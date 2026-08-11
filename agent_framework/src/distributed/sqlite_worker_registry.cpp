#include "agent/distributed/durable_queue.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::distributed
{
    namespace sqlite = agent_framework::internal::sqlite;
    SQLiteWorkerRegistry::SQLiteWorkerRegistry(std::string path, int timeout)
    {
        if (path.empty())
            throw std::invalid_argument("worker registry path required");
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
    SQLiteWorkerRegistry::~SQLiteWorkerRegistry()
    {
        if (db_)
            sqlite3_close(sqlite::database(db_));
    }
    void SQLiteWorkerRegistry::migrate()
    {
        auto *db = sqlite::database(db_);
        sqlite::exec(db, "PRAGMA journal_mode=WAL");
        sqlite::exec(db, "PRAGMA synchronous=FULL");
        sqlite::exec(db, "CREATE TABLE IF NOT EXISTS worker_registry(worker_id TEXT PRIMARY KEY,instance_id TEXT NOT NULL,capabilities_digest TEXT NOT NULL,generation INTEGER NOT NULL,heartbeat_at_ms INTEGER NOT NULL)");
        sqlite::exec(db, "CREATE TABLE IF NOT EXISTS tenant_quota(tenant_id TEXT PRIMARY KEY,maximum_active INTEGER NOT NULL,active INTEGER NOT NULL CHECK(active>=0 AND active<=maximum_active))");
    }
    std::optional<std::uint64_t> SQLiteWorkerRegistry::register_worker(const WorkerRecord &v, std::string *error)
    {
        if (v.worker_id.empty() || v.instance_id.empty() || v.capabilities_digest.empty())
        {
            if (error)
                *error = "worker identity and capabilities required";
            return std::nullopt;
        }
        std::lock_guard l(mutex_);
        try
        {
            auto *db = sqlite::database(db_);
            sqlite::Transaction tx(db);
            std::uint64_t generation = 1;
            sqlite::Statement read(db, "SELECT generation FROM worker_registry WHERE worker_id=?");
            sqlite::bind_text(read.get(), 1, v.worker_id);
            if (sqlite::step(read.get()) == SQLITE_ROW)
                generation = sqlite::column_uint64(read.get(), 0) + 1;
            sqlite::Statement write(db, "INSERT INTO worker_registry VALUES(?,?,?,?,?) ON CONFLICT(worker_id) DO UPDATE SET instance_id=excluded.instance_id,capabilities_digest=excluded.capabilities_digest,generation=excluded.generation,heartbeat_at_ms=excluded.heartbeat_at_ms");
            sqlite::bind_text(write.get(), 1, v.worker_id);
            sqlite::bind_text(write.get(), 2, v.instance_id);
            sqlite::bind_text(write.get(), 3, v.capabilities_digest);
            sqlite::bind_uint64(write.get(), 4, generation);
            sqlite::bind_int64(write.get(), 5, v.heartbeat_at_ms);
            if (sqlite::step(write.get()) != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(db));
            tx.commit();
            return generation;
        }
        catch (const std::exception &x)
        {
            if (error)
                *error = x.what();
            return std::nullopt;
        }
    }
    bool SQLiteWorkerRegistry::heartbeat(std::string_view worker, std::string_view instance, std::uint64_t generation, std::int64_t now)
    {
        std::lock_guard l(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "UPDATE worker_registry SET heartbeat_at_ms=? WHERE worker_id=? AND instance_id=? AND generation=? AND heartbeat_at_ms<=?");
        sqlite::bind_int64(s.get(), 1, now);
        sqlite::bind_text(s.get(), 2, worker);
        sqlite::bind_text(s.get(), 3, instance);
        sqlite::bind_uint64(s.get(), 4, generation);
        sqlite::bind_int64(s.get(), 5, now);
        return sqlite::step(s.get()) == SQLITE_DONE && sqlite::changes(db) == 1;
    }
    std::vector<WorkerRecord> SQLiteWorkerRegistry::alive(std::int64_t threshold) const
    {
        std::lock_guard l(mutex_);
        std::vector<WorkerRecord> out;
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "SELECT worker_id,instance_id,capabilities_digest,generation,heartbeat_at_ms FROM worker_registry WHERE heartbeat_at_ms>=? ORDER BY worker_id");
        sqlite::bind_int64(s.get(), 1, threshold);
        while (sqlite::step(s.get()) == SQLITE_ROW)
            out.push_back({sqlite::column_text(s.get(), 0), sqlite::column_text(s.get(), 1), sqlite::column_text(s.get(), 2), sqlite::column_uint64(s.get(), 3), sqlite::column_int64(s.get(), 4)});
        return out;
    }
    bool SQLiteWorkerRegistry::set_quota(std::string_view tenant, std::uint64_t maximum)
    {
        if (tenant.empty() || maximum == 0)
            return false;
        std::lock_guard l(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "INSERT INTO tenant_quota VALUES(?,?,0) ON CONFLICT(tenant_id) DO UPDATE SET maximum_active=excluded.maximum_active WHERE active<=excluded.maximum_active");
        sqlite::bind_text(s.get(), 1, tenant);
        sqlite::bind_uint64(s.get(), 2, maximum);
        return sqlite::step(s.get()) == SQLITE_DONE && sqlite::changes(db) == 1;
    }
    bool SQLiteWorkerRegistry::reserve(std::string_view tenant)
    {
        std::lock_guard l(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "UPDATE tenant_quota SET active=active+1 WHERE tenant_id=? AND active<maximum_active");
        sqlite::bind_text(s.get(), 1, tenant);
        return sqlite::step(s.get()) == SQLITE_DONE && sqlite::changes(db) == 1;
    }
    bool SQLiteWorkerRegistry::release(std::string_view tenant)
    {
        std::lock_guard l(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "UPDATE tenant_quota SET active=active-1 WHERE tenant_id=? AND active>0");
        sqlite::bind_text(s.get(), 1, tenant);
        return sqlite::step(s.get()) == SQLITE_DONE && sqlite::changes(db) == 1;
    }
    std::optional<TenantQuota> SQLiteWorkerRegistry::quota(std::string_view tenant) const
    {
        std::lock_guard l(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "SELECT maximum_active,active FROM tenant_quota WHERE tenant_id=?");
        sqlite::bind_text(s.get(), 1, tenant);
        if (sqlite::step(s.get()) != SQLITE_ROW)
            return std::nullopt;
        return TenantQuota{std::string(tenant), sqlite::column_uint64(s.get(), 0), sqlite::column_uint64(s.get(), 1)};
    }
} // namespace agent_framework::distributed
