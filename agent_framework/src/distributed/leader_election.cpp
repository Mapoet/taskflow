#include "agent/distributed/leader_election.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::distributed
{
    namespace sqlite = agent_framework::internal::sqlite;
    SQLiteLeaderElection::SQLiteLeaderElection(std::string path, int timeout)
    {
        if (path.empty())
            throw std::invalid_argument("leader database path required");
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
    SQLiteLeaderElection::~SQLiteLeaderElection()
    {
        if (db_)
            sqlite3_close(sqlite::database(db_));
    }
    void SQLiteLeaderElection::migrate()
    {
        auto *db = sqlite::database(db_);
        sqlite::exec(db, "PRAGMA journal_mode=WAL");
        sqlite::exec(db, "PRAGMA synchronous=FULL");
        sqlite::exec(db, "CREATE TABLE IF NOT EXISTS leader_election(election TEXT PRIMARY KEY,owner TEXT NOT NULL,fencing_token INTEGER NOT NULL,expires_at_ms INTEGER NOT NULL)");
    }
    std::optional<LeaderLease> SQLiteLeaderElection::acquire(std::string_view election, std::string_view owner, std::int64_t now, std::int64_t lease_ms)
    {
        if (election.empty() || owner.empty() || lease_ms <= 0)
            return std::nullopt;
        std::lock_guard l(mutex_);
        try
        {
            auto *db = sqlite::database(db_);
            sqlite::Transaction tx(db);
            sqlite::Statement insert(db, "INSERT OR IGNORE INTO leader_election VALUES(?,'',0,0)");
            sqlite::bind_text(insert.get(), 1, election);
            if (sqlite::step(insert.get()) != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(db));
            sqlite::Statement update(db, "UPDATE leader_election SET owner=?,fencing_token=fencing_token+1,expires_at_ms=? WHERE election=? AND (owner='' OR expires_at_ms<=?)");
            sqlite::bind_text(update.get(), 1, owner);
            sqlite::bind_int64(update.get(), 2, now + lease_ms);
            sqlite::bind_text(update.get(), 3, election);
            sqlite::bind_int64(update.get(), 4, now);
            if (sqlite::step(update.get()) != SQLITE_DONE || sqlite::changes(db) != 1)
                return std::nullopt;
            sqlite::Statement read(db, "SELECT fencing_token,expires_at_ms FROM leader_election WHERE election=?");
            sqlite::bind_text(read.get(), 1, election);
            if (sqlite::step(read.get()) != SQLITE_ROW)
                throw std::runtime_error("leader row disappeared");
            LeaderLease result{std::string(election), std::string(owner), sqlite::column_uint64(read.get(), 0), sqlite::column_int64(read.get(), 1)};
            tx.commit();
            return result;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
    bool SQLiteLeaderElection::renew(const LeaderLease &v, std::int64_t now, std::int64_t lease_ms)
    {
        if (lease_ms <= 0)
            return false;
        std::lock_guard l(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "UPDATE leader_election SET expires_at_ms=? WHERE election=? AND owner=? AND fencing_token=? AND expires_at_ms>?");
        sqlite::bind_int64(s.get(), 1, now + lease_ms);
        sqlite::bind_text(s.get(), 2, v.election);
        sqlite::bind_text(s.get(), 3, v.owner);
        sqlite::bind_uint64(s.get(), 4, v.fencing_token);
        sqlite::bind_int64(s.get(), 5, now);
        return sqlite::step(s.get()) == SQLITE_DONE && sqlite::changes(db) == 1;
    }
    bool SQLiteLeaderElection::release(const LeaderLease &v)
    {
        std::lock_guard l(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "UPDATE leader_election SET owner='',expires_at_ms=0 WHERE election=? AND owner=? AND fencing_token=?");
        sqlite::bind_text(s.get(), 1, v.election);
        sqlite::bind_text(s.get(), 2, v.owner);
        sqlite::bind_uint64(s.get(), 3, v.fencing_token);
        return sqlite::step(s.get()) == SQLITE_DONE && sqlite::changes(db) == 1;
    }
    bool SQLiteLeaderElection::is_current(const LeaderLease &v, std::int64_t now) const
    {
        std::lock_guard l(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "SELECT 1 FROM leader_election WHERE election=? AND owner=? AND fencing_token=? AND expires_at_ms>?");
        sqlite::bind_text(s.get(), 1, v.election);
        sqlite::bind_text(s.get(), 2, v.owner);
        sqlite::bind_uint64(s.get(), 3, v.fencing_token);
        sqlite::bind_int64(s.get(), 4, now);
        return sqlite::step(s.get()) == SQLITE_ROW;
    }
    std::optional<LeaderLease> SQLiteLeaderElection::inspect(std::string_view election) const
    {
        std::lock_guard l(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "SELECT owner,fencing_token,expires_at_ms FROM leader_election WHERE election=?");
        sqlite::bind_text(s.get(), 1, election);
        if (sqlite::step(s.get()) != SQLITE_ROW)
            return std::nullopt;
        return LeaderLease{std::string(election), sqlite::column_text(s.get(), 0), sqlite::column_uint64(s.get(), 1), sqlite::column_int64(s.get(), 2)};
    }
} // namespace agent_framework::distributed
