#include "agent/distributed/schema_migration.hpp"

#include <filesystem>
#include <limits>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::distributed
{
    namespace sqlite = agent_framework::internal::sqlite;
    namespace
    {
        ComponentSchema row(sqlite3_stmt *s) { return {sqlite::column_text(s, 0), sqlite::column_uint64(s, 1), sqlite::column_uint64(s, 2), sqlite::column_uint64(s, 3), sqlite::column_text(s, 4), sqlite::column_int64(s, 5)}; }
        std::optional<std::int64_t> lease_expiry(std::int64_t now, std::int64_t lease_ms)
        {
            if (now < 0 || lease_ms <= 0 ||
                now > std::numeric_limits<std::int64_t>::max() - lease_ms)
                return std::nullopt;
            return now + lease_ms;
        }
    }
    SQLiteSchemaMigrationCoordinator::SQLiteSchemaMigrationCoordinator(std::string path, int timeout)
    {
        if (path.empty())
            throw std::invalid_argument("schema database path required");
        std::error_code ec;
        std::filesystem::path p(path);
        if (p.has_parent_path())
            std::filesystem::create_directories(p.parent_path(), ec);
        sqlite3 *opened = nullptr;
        if (ec || sqlite3_open_v2(path.c_str(), &opened, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
        {
            auto message = ec ? ec.message() : (opened ? sqlite3_errmsg(opened) : "sqlite open failed");
            if (opened)
                sqlite3_close(opened);
            throw std::runtime_error(message);
        }
        db_ = opened;
        sqlite3_busy_timeout(opened, timeout);
        migrate();
    }
    SQLiteSchemaMigrationCoordinator::~SQLiteSchemaMigrationCoordinator()
    {
        if (db_)
            sqlite3_close(sqlite::database(db_));
    }
    void SQLiteSchemaMigrationCoordinator::migrate()
    {
        auto *db = sqlite::database(db_);
        sqlite::exec(db, "PRAGMA journal_mode=WAL");
        sqlite::exec(db, "PRAGMA synchronous=FULL");
        sqlite::exec(db, "CREATE TABLE IF NOT EXISTS component_schema(component TEXT PRIMARY KEY,version INTEGER NOT NULL,minimum_reader_version INTEGER NOT NULL,migration_fencing_token INTEGER NOT NULL,migration_owner TEXT NOT NULL,migration_expires_at_ms INTEGER NOT NULL)");
    }
    bool SQLiteSchemaMigrationCoordinator::initialize(std::string_view component, std::uint64_t version, std::uint64_t minimum, std::string *error)
    {
        if (component.empty() || version == 0 || minimum == 0 || minimum > version)
        {
            if (error)
                *error = "invalid schema version";
            return false;
        }
        std::lock_guard l(mutex_);
        try
        {
            auto *db = sqlite::database(db_);
            sqlite::Statement s(db, "INSERT INTO component_schema VALUES(?,?,?,0,'',0)");
            sqlite::bind_text(s.get(), 1, component);
            sqlite::bind_uint64(s.get(), 2, version);
            sqlite::bind_uint64(s.get(), 3, minimum);
            if (sqlite::step(s.get()) != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(db));
            return true;
        }
        catch (const std::exception &x)
        {
            if (error)
                *error = x.what();
            return false;
        }
    }
    std::optional<ComponentSchema> SQLiteSchemaMigrationCoordinator::inspect(std::string_view component) const
    {
        std::lock_guard l(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "SELECT component,version,minimum_reader_version,migration_fencing_token,migration_owner,migration_expires_at_ms FROM component_schema WHERE component=?");
        sqlite::bind_text(s.get(), 1, component);
        return sqlite::step(s.get()) == SQLITE_ROW ? std::optional<ComponentSchema>(row(s.get())) : std::nullopt;
    }
    bool SQLiteSchemaMigrationCoordinator::reader_compatible(std::string_view component, std::uint64_t reader, std::string *error) const
    {
        auto schema = inspect(component);
        if (!schema)
        {
            if (error)
                *error = "component schema missing";
            return false;
        }
        if (reader < schema->minimum_reader_version)
        {
            if (error)
                *error = "reader version is below schema compatibility floor";
            return false;
        }
        return true;
    }
    std::optional<MigrationLease> SQLiteSchemaMigrationCoordinator::begin(std::string_view component, std::uint64_t expected, std::string_view owner, std::int64_t now, std::int64_t lease_ms, std::string *error)
    {
        const auto expires_at = lease_expiry(now, lease_ms);
        if (component.empty() || owner.empty() || !expires_at)
            return std::nullopt;
        std::lock_guard l(mutex_);
        try
        {
            auto *db = sqlite::database(db_);
            sqlite::Transaction tx(db);
            sqlite::Statement update(db, "UPDATE component_schema SET migration_owner=?,migration_expires_at_ms=?,migration_fencing_token=migration_fencing_token+1 WHERE component=? AND version=? AND (migration_owner='' OR migration_expires_at_ms<=?)");
            sqlite::bind_text(update.get(), 1, owner);
            sqlite::bind_int64(update.get(), 2, *expires_at);
            sqlite::bind_text(update.get(), 3, component);
            sqlite::bind_uint64(update.get(), 4, expected);
            sqlite::bind_int64(update.get(), 5, now);
            if (sqlite::step(update.get()) != SQLITE_DONE || sqlite::changes(db) != 1)
            {
                if (error)
                    *error = "migration lease unavailable or schema changed";
                return std::nullopt;
            }
            sqlite::Statement read(db, "SELECT component,version,minimum_reader_version,migration_fencing_token,migration_owner,migration_expires_at_ms FROM component_schema WHERE component=?");
            sqlite::bind_text(read.get(), 1, component);
            if (sqlite::step(read.get()) != SQLITE_ROW)
                throw std::runtime_error("schema disappeared");
            auto schema = row(read.get());
            tx.commit();
            return MigrationLease{schema, schema.migration_fencing_token};
        }
        catch (const std::exception &x)
        {
            if (error)
                *error = x.what();
            return std::nullopt;
        }
    }
    bool SQLiteSchemaMigrationCoordinator::renew(std::string_view component, std::string_view owner, std::uint64_t token, std::int64_t now, std::int64_t lease_ms)
    {
        const auto expires_at = lease_expiry(now, lease_ms);
        if (!expires_at)
            return false;
        std::lock_guard l(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "UPDATE component_schema SET migration_expires_at_ms=? WHERE component=? AND migration_owner=? AND migration_fencing_token=? AND migration_expires_at_ms>?");
        sqlite::bind_int64(s.get(), 1, *expires_at);
        sqlite::bind_text(s.get(), 2, component);
        sqlite::bind_text(s.get(), 3, owner);
        sqlite::bind_uint64(s.get(), 4, token);
        sqlite::bind_int64(s.get(), 5, now);
        return sqlite::step(s.get()) == SQLITE_DONE && sqlite::changes(db) == 1;
    }
    bool SQLiteSchemaMigrationCoordinator::commit(std::string_view component, std::string_view owner, std::uint64_t token, std::uint64_t version, std::uint64_t minimum, std::string *error)
    {
        if (version == 0 || minimum == 0 || minimum > version)
        {
            if (error)
                *error = "invalid target schema version";
            return false;
        }
        std::lock_guard l(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "UPDATE component_schema SET version=?,minimum_reader_version=?,migration_owner='',migration_expires_at_ms=0 WHERE component=? AND migration_owner=? AND migration_fencing_token=? AND version<?");
        sqlite::bind_uint64(s.get(), 1, version);
        sqlite::bind_uint64(s.get(), 2, minimum);
        sqlite::bind_text(s.get(), 3, component);
        sqlite::bind_text(s.get(), 4, owner);
        sqlite::bind_uint64(s.get(), 5, token);
        sqlite::bind_uint64(s.get(), 6, version);
        const bool ok = sqlite::step(s.get()) == SQLITE_DONE && sqlite::changes(db) == 1;
        if (!ok && error)
            *error = "stale migration token or non-forward migration";
        return ok;
    }
} // namespace agent_framework::distributed
