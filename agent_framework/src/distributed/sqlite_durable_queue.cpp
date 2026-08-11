#include "agent/distributed/durable_queue.hpp"

#include <filesystem>
#include <limits>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::distributed
{
    namespace
    {
        namespace sqlite = agent_framework::internal::sqlite;
        int state_value(QueueState value) { return static_cast<int>(value); }
        bool valid_lease_time(std::int64_t now, std::int64_t lease_ms)
        {
            return now >= 0 && lease_ms > 0 &&
                   now <= std::numeric_limits<std::int64_t>::max() - lease_ms;
        }
        QueueTask row(sqlite3_stmt *s)
        {
            QueueTask v;
            v.task_id = sqlite::column_text(s, 0);
            v.tenant_id = sqlite::column_text(s, 1);
            v.idempotency_key = sqlite::column_text(s, 2);
            v.payload_digest = sqlite::column_text(s, 3);
            v.priority = sqlite::column_int(s, 4);
            v.available_at_ms = sqlite::column_int64(s, 5);
            v.attempts = static_cast<std::uint32_t>(sqlite::column_int64(s, 6));
            v.max_attempts = static_cast<std::uint32_t>(sqlite::column_int64(s, 7));
            v.state = static_cast<QueueState>(sqlite::column_int(s, 8));
            v.owner = sqlite::column_text(s, 9);
            v.fencing_token = sqlite::column_uint64(s, 10);
            v.lease_expires_at_ms = sqlite::column_int64(s, 11);
            return v;
        }
        constexpr auto columns = "task_id,tenant_id,idempotency_key,payload_digest,priority,available_at_ms,attempts,max_attempts,state,owner,fencing_token,lease_expires_at_ms";
    }
    SQLiteDurableQueue::SQLiteDurableQueue(std::string path, int timeout)
    {
        if (path.empty())
            throw std::invalid_argument("queue path required");
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
    SQLiteDurableQueue::~SQLiteDurableQueue()
    {
        if (db_)
            sqlite3_close(sqlite::database(db_));
    }
    void SQLiteDurableQueue::migrate()
    {
        auto *db = sqlite::database(db_);
        sqlite::exec(db, "PRAGMA journal_mode=WAL");
        sqlite::exec(db, "PRAGMA synchronous=FULL");
        sqlite::exec(db, "CREATE TABLE IF NOT EXISTS durable_queue(task_id TEXT PRIMARY KEY,tenant_id TEXT NOT NULL,idempotency_key TEXT NOT NULL,payload_digest TEXT NOT NULL,priority INTEGER NOT NULL,available_at_ms INTEGER NOT NULL,attempts INTEGER NOT NULL,max_attempts INTEGER NOT NULL,state INTEGER NOT NULL,owner TEXT NOT NULL,fencing_token INTEGER NOT NULL,lease_expires_at_ms INTEGER NOT NULL,UNIQUE(tenant_id,idempotency_key))");
        sqlite::exec(db, "CREATE INDEX IF NOT EXISTS durable_queue_claim ON durable_queue(tenant_id,state,available_at_ms,priority)");
        sqlite::exec(db, "CREATE TABLE IF NOT EXISTS tenant_quota(tenant_id TEXT PRIMARY KEY,maximum_active INTEGER NOT NULL,active INTEGER NOT NULL CHECK(active>=0 AND active<=maximum_active))");
    }
    bool SQLiteDurableQueue::enqueue(QueueTask v, std::string *error)
    {
        if (v.task_id.empty() || v.tenant_id.empty() || v.idempotency_key.empty() || v.payload_digest.empty() || !v.max_attempts)
        {
            if (error)
                *error = "queue task identity, payload, and attempts are required";
            return false;
        }
        std::lock_guard l(mutex_);
        try
        {
            auto *db = sqlite::database(db_);
            sqlite::Transaction tx(db);
            sqlite::Statement s(db, "INSERT OR IGNORE INTO durable_queue VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
            sqlite::bind_text(s.get(), 1, v.task_id);
            sqlite::bind_text(s.get(), 2, v.tenant_id);
            sqlite::bind_text(s.get(), 3, v.idempotency_key);
            sqlite::bind_text(s.get(), 4, v.payload_digest);
            sqlite::bind_int(s.get(), 5, v.priority);
            sqlite::bind_int64(s.get(), 6, v.available_at_ms);
            sqlite::bind_int64(s.get(), 7, 0);
            sqlite::bind_int64(s.get(), 8, v.max_attempts);
            sqlite::bind_int(s.get(), 9, state_value(QueueState::Pending));
            sqlite::bind_text(s.get(), 10, "");
            sqlite::bind_uint64(s.get(), 11, 0);
            sqlite::bind_int64(s.get(), 12, 0);
            if (sqlite::step(s.get()) != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(db));
            if (sqlite::changes(db) == 1)
            {
                tx.commit();
                return true;
            }
            sqlite::Statement existing(db,
                                       "SELECT task_id,tenant_id,idempotency_key,payload_digest,priority,available_at_ms,max_attempts "
                                       "FROM durable_queue WHERE task_id=? OR (tenant_id=? AND idempotency_key=?)");
            sqlite::bind_text(existing.get(), 1, v.task_id);
            sqlite::bind_text(existing.get(), 2, v.tenant_id);
            sqlite::bind_text(existing.get(), 3, v.idempotency_key);
            const bool replay = sqlite::step(existing.get()) == SQLITE_ROW &&
                                sqlite::column_text(existing.get(), 0) == v.task_id &&
                                sqlite::column_text(existing.get(), 1) == v.tenant_id &&
                                sqlite::column_text(existing.get(), 2) == v.idempotency_key &&
                                sqlite::column_text(existing.get(), 3) == v.payload_digest &&
                                sqlite::column_int(existing.get(), 4) == v.priority &&
                                sqlite::column_int64(existing.get(), 5) == v.available_at_ms &&
                                sqlite::column_uint64(existing.get(), 6) == v.max_attempts;
            tx.commit();
            if (!replay && error)
                *error = "queue idempotency conflict";
            return replay;
        }
        catch (const std::exception &x)
        {
            if (error)
                *error = x.what();
            return false;
        }
    }
    std::optional<Lease> SQLiteDurableQueue::claim(std::string_view worker, std::string_view tenant, std::int64_t now, std::int64_t lease_ms)
    {
        if (worker.empty() || tenant.empty() || !valid_lease_time(now, lease_ms))
            return std::nullopt;
        std::lock_guard l(mutex_);
        try
        {
            auto *db = sqlite::database(db_);
            sqlite::Transaction tx(db);
            sqlite::Statement dead(db, "UPDATE durable_queue SET state=?,owner='',lease_expires_at_ms=0 WHERE tenant_id=? AND state IN (?,?) AND available_at_ms<=? AND (state=? AND lease_expires_at_ms<=? OR state=?) AND attempts>=max_attempts");
            sqlite::bind_int(dead.get(), 1, state_value(QueueState::DeadLetter));
            sqlite::bind_text(dead.get(), 2, tenant);
            sqlite::bind_int(dead.get(), 3, state_value(QueueState::Pending));
            sqlite::bind_int(dead.get(), 4, state_value(QueueState::Leased));
            sqlite::bind_int64(dead.get(), 5, now);
            sqlite::bind_int(dead.get(), 6, state_value(QueueState::Leased));
            sqlite::bind_int64(dead.get(), 7, now);
            sqlite::bind_int(dead.get(), 8, state_value(QueueState::Pending));
            if (sqlite::step(dead.get()) != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(db));
            const std::string sql = std::string("SELECT ") + columns + " FROM durable_queue WHERE tenant_id=? AND available_at_ms<=? AND attempts<max_attempts AND (state=? OR (state=? AND lease_expires_at_ms<=?)) ORDER BY priority DESC,task_id LIMIT 1";
            sqlite::Statement select(db, sql.c_str());
            sqlite::bind_text(select.get(), 1, tenant);
            sqlite::bind_int64(select.get(), 2, now);
            sqlite::bind_int(select.get(), 3, state_value(QueueState::Pending));
            sqlite::bind_int(select.get(), 4, state_value(QueueState::Leased));
            sqlite::bind_int64(select.get(), 5, now);
            if (sqlite::step(select.get()) != SQLITE_ROW)
            {
                tx.commit();
                return std::nullopt;
            }
            auto task = row(select.get());
            sqlite::Statement update(db, "UPDATE durable_queue SET state=?,owner=?,fencing_token=fencing_token+1,attempts=attempts+1,lease_expires_at_ms=? WHERE task_id=? AND fencing_token=?");
            sqlite::bind_int(update.get(), 1, state_value(QueueState::Leased));
            sqlite::bind_text(update.get(), 2, worker);
            sqlite::bind_int64(update.get(), 3, now + lease_ms);
            sqlite::bind_text(update.get(), 4, task.task_id);
            sqlite::bind_uint64(update.get(), 5, task.fencing_token);
            if (sqlite::step(update.get()) != SQLITE_DONE || sqlite::changes(db) != 1)
                throw std::runtime_error("queue claim conflict");
            ++task.fencing_token;
            ++task.attempts;
            task.state = QueueState::Leased;
            task.owner = std::string(worker);
            task.lease_expires_at_ms = now + lease_ms;
            tx.commit();
            return Lease{task, task.fencing_token};
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
    std::optional<Lease> SQLiteDurableQueue::claim_with_quota(std::string_view worker, std::string_view tenant, std::int64_t now, std::int64_t lease_ms)
    {
        if (worker.empty() || tenant.empty() || !valid_lease_time(now, lease_ms))
            return std::nullopt;
        std::lock_guard l(mutex_);
        try
        {
            auto *db = sqlite::database(db_);
            sqlite::Transaction tx(db);
            sqlite::Statement expired_count(db, "SELECT count(*) FROM durable_queue WHERE tenant_id=? AND state=? AND lease_expires_at_ms<=? AND attempts>=max_attempts");
            sqlite::bind_text(expired_count.get(), 1, tenant);
            sqlite::bind_int(expired_count.get(), 2, state_value(QueueState::Leased));
            sqlite::bind_int64(expired_count.get(), 3, now);
            if (sqlite::step(expired_count.get()) != SQLITE_ROW)
                throw std::runtime_error(sqlite3_errmsg(db));
            const auto release_count = sqlite::column_int64(expired_count.get(), 0);
            sqlite::Statement dead(db, "UPDATE durable_queue SET state=?,owner='',lease_expires_at_ms=0 WHERE tenant_id=? AND state=? AND lease_expires_at_ms<=? AND attempts>=max_attempts");
            sqlite::bind_int(dead.get(), 1, state_value(QueueState::DeadLetter));
            sqlite::bind_text(dead.get(), 2, tenant);
            sqlite::bind_int(dead.get(), 3, state_value(QueueState::Leased));
            sqlite::bind_int64(dead.get(), 4, now);
            if (sqlite::step(dead.get()) != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(db));
            if (release_count > 0)
            {
                sqlite::Statement release(db, "UPDATE tenant_quota SET active=active-? WHERE tenant_id=? AND active>=?");
                sqlite::bind_int64(release.get(), 1, release_count);
                sqlite::bind_text(release.get(), 2, tenant);
                sqlite::bind_int64(release.get(), 3, release_count);
                if (sqlite::step(release.get()) != SQLITE_DONE || sqlite::changes(db) != 1)
                    throw std::runtime_error("quota reconciliation failed");
            }
            const std::string sql = std::string("SELECT ") + columns + " FROM durable_queue WHERE tenant_id=? AND available_at_ms<=? AND attempts<max_attempts AND (state=? OR (state=? AND lease_expires_at_ms<=?)) ORDER BY priority DESC,task_id LIMIT 1";
            sqlite::Statement select(db, sql.c_str());
            sqlite::bind_text(select.get(), 1, tenant);
            sqlite::bind_int64(select.get(), 2, now);
            sqlite::bind_int(select.get(), 3, state_value(QueueState::Pending));
            sqlite::bind_int(select.get(), 4, state_value(QueueState::Leased));
            sqlite::bind_int64(select.get(), 5, now);
            if (sqlite::step(select.get()) != SQLITE_ROW)
            {
                tx.commit();
                return std::nullopt;
            }
            auto task = row(select.get());
            if (task.state == QueueState::Pending)
            {
                sqlite::Statement reserve(db, "UPDATE tenant_quota SET active=active+1 WHERE tenant_id=? AND active<maximum_active");
                sqlite::bind_text(reserve.get(), 1, tenant);
                if (sqlite::step(reserve.get()) != SQLITE_DONE || sqlite::changes(db) != 1)
                {
                    tx.commit();
                    return std::nullopt;
                }
            }
            sqlite::Statement update(db, "UPDATE durable_queue SET state=?,owner=?,fencing_token=fencing_token+1,attempts=attempts+1,lease_expires_at_ms=? WHERE task_id=? AND fencing_token=?");
            sqlite::bind_int(update.get(), 1, state_value(QueueState::Leased));
            sqlite::bind_text(update.get(), 2, worker);
            sqlite::bind_int64(update.get(), 3, now + lease_ms);
            sqlite::bind_text(update.get(), 4, task.task_id);
            sqlite::bind_uint64(update.get(), 5, task.fencing_token);
            if (sqlite::step(update.get()) != SQLITE_DONE || sqlite::changes(db) != 1)
                throw std::runtime_error("queue claim conflict");
            ++task.fencing_token;
            ++task.attempts;
            task.state = QueueState::Leased;
            task.owner = std::string(worker);
            task.lease_expires_at_ms = now + lease_ms;
            tx.commit();
            return Lease{task, task.fencing_token};
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
    bool SQLiteDurableQueue::renew(std::string_view id, std::string_view worker, std::uint64_t token, std::int64_t now, std::int64_t lease_ms)
    {
        if (!valid_lease_time(now, lease_ms))
            return false;
        std::lock_guard l(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "UPDATE durable_queue SET lease_expires_at_ms=? WHERE task_id=? AND state=? AND owner=? AND fencing_token=? AND lease_expires_at_ms>?");
        sqlite::bind_int64(s.get(), 1, now + lease_ms);
        sqlite::bind_text(s.get(), 2, id);
        sqlite::bind_int(s.get(), 3, state_value(QueueState::Leased));
        sqlite::bind_text(s.get(), 4, worker);
        sqlite::bind_uint64(s.get(), 5, token);
        sqlite::bind_int64(s.get(), 6, now);
        return sqlite::step(s.get()) == SQLITE_DONE && sqlite::changes(db) == 1;
    }
    bool SQLiteDurableQueue::ack(std::string_view id, std::string_view worker, std::uint64_t token)
    {
        std::lock_guard l(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "UPDATE durable_queue SET state=?,owner='',lease_expires_at_ms=0 WHERE task_id=? AND state=? AND owner=? AND fencing_token=?");
        sqlite::bind_int(s.get(), 1, state_value(QueueState::Completed));
        sqlite::bind_text(s.get(), 2, id);
        sqlite::bind_int(s.get(), 3, state_value(QueueState::Leased));
        sqlite::bind_text(s.get(), 4, worker);
        sqlite::bind_uint64(s.get(), 5, token);
        return sqlite::step(s.get()) == SQLITE_DONE && sqlite::changes(db) == 1;
    }
    bool SQLiteDurableQueue::ack_with_quota(std::string_view id, std::string_view worker, std::uint64_t token)
    {
        std::lock_guard l(mutex_);
        try
        {
            auto *db = sqlite::database(db_);
            sqlite::Transaction tx(db);
            sqlite::Statement read(db, "SELECT tenant_id FROM durable_queue WHERE task_id=? AND state=? AND owner=? AND fencing_token=?");
            sqlite::bind_text(read.get(), 1, id);
            sqlite::bind_int(read.get(), 2, state_value(QueueState::Leased));
            sqlite::bind_text(read.get(), 3, worker);
            sqlite::bind_uint64(read.get(), 4, token);
            if (sqlite::step(read.get()) != SQLITE_ROW)
                return false;
            const auto tenant = sqlite::column_text(read.get(), 0);
            sqlite::Statement done(db, "UPDATE durable_queue SET state=?,owner='',lease_expires_at_ms=0 WHERE task_id=? AND state=? AND owner=? AND fencing_token=?");
            sqlite::bind_int(done.get(), 1, state_value(QueueState::Completed));
            sqlite::bind_text(done.get(), 2, id);
            sqlite::bind_int(done.get(), 3, state_value(QueueState::Leased));
            sqlite::bind_text(done.get(), 4, worker);
            sqlite::bind_uint64(done.get(), 5, token);
            if (sqlite::step(done.get()) != SQLITE_DONE || sqlite::changes(db) != 1)
                throw std::runtime_error("queue ack conflict");
            sqlite::Statement release(db, "UPDATE tenant_quota SET active=active-1 WHERE tenant_id=? AND active>0");
            sqlite::bind_text(release.get(), 1, tenant);
            if (sqlite::step(release.get()) != SQLITE_DONE || sqlite::changes(db) != 1)
                throw std::runtime_error("quota release failed");
            tx.commit();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
    bool SQLiteDurableQueue::ack_with_quota_at(std::string_view id, std::string_view worker,
                                                std::uint64_t token, std::int64_t now)
    {
        if (now < 0)
            return false;
        std::lock_guard l(mutex_);
        try
        {
            auto *db = sqlite::database(db_);
            sqlite::Transaction tx(db);
            sqlite::Statement read(db, "SELECT tenant_id FROM durable_queue WHERE task_id=? AND state=? AND owner=? AND fencing_token=? AND lease_expires_at_ms>?");
            sqlite::bind_text(read.get(), 1, id);
            sqlite::bind_int(read.get(), 2, state_value(QueueState::Leased));
            sqlite::bind_text(read.get(), 3, worker);
            sqlite::bind_uint64(read.get(), 4, token);
            sqlite::bind_int64(read.get(), 5, now);
            if (sqlite::step(read.get()) != SQLITE_ROW)
                return false;
            const auto tenant = sqlite::column_text(read.get(), 0);
            sqlite::Statement done(db, "UPDATE durable_queue SET state=?,owner='',lease_expires_at_ms=0 WHERE task_id=? AND state=? AND owner=? AND fencing_token=? AND lease_expires_at_ms>?");
            sqlite::bind_int(done.get(), 1, state_value(QueueState::Completed));
            sqlite::bind_text(done.get(), 2, id);
            sqlite::bind_int(done.get(), 3, state_value(QueueState::Leased));
            sqlite::bind_text(done.get(), 4, worker);
            sqlite::bind_uint64(done.get(), 5, token);
            sqlite::bind_int64(done.get(), 6, now);
            if (sqlite::step(done.get()) != SQLITE_DONE || sqlite::changes(db) != 1)
                throw std::runtime_error("queue ack conflict");
            sqlite::Statement release(db, "UPDATE tenant_quota SET active=active-1 WHERE tenant_id=? AND active>0");
            sqlite::bind_text(release.get(), 1, tenant);
            if (sqlite::step(release.get()) != SQLITE_DONE || sqlite::changes(db) != 1)
                throw std::runtime_error("quota release failed");
            tx.commit();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
    bool SQLiteDurableQueue::nack(std::string_view id, std::string_view worker, std::uint64_t token, std::int64_t available)
    {
        std::lock_guard l(mutex_);
        auto *db = sqlite::database(db_);
        sqlite::Statement s(db, "UPDATE durable_queue SET state=CASE WHEN attempts>=max_attempts THEN ? ELSE ? END,owner='',lease_expires_at_ms=0,available_at_ms=? WHERE task_id=? AND state=? AND owner=? AND fencing_token=?");
        sqlite::bind_int(s.get(), 1, state_value(QueueState::DeadLetter));
        sqlite::bind_int(s.get(), 2, state_value(QueueState::Pending));
        sqlite::bind_int64(s.get(), 3, available);
        sqlite::bind_text(s.get(), 4, id);
        sqlite::bind_int(s.get(), 5, state_value(QueueState::Leased));
        sqlite::bind_text(s.get(), 6, worker);
        sqlite::bind_uint64(s.get(), 7, token);
        return sqlite::step(s.get()) == SQLITE_DONE && sqlite::changes(db) == 1;
    }
    bool SQLiteDurableQueue::nack_with_quota(std::string_view id, std::string_view worker, std::uint64_t token, std::int64_t available)
    {
        std::lock_guard l(mutex_);
        try
        {
            auto *db = sqlite::database(db_);
            sqlite::Transaction tx(db);
            sqlite::Statement read(db, "SELECT tenant_id FROM durable_queue WHERE task_id=? AND state=? AND owner=? AND fencing_token=?");
            sqlite::bind_text(read.get(), 1, id);
            sqlite::bind_int(read.get(), 2, state_value(QueueState::Leased));
            sqlite::bind_text(read.get(), 3, worker);
            sqlite::bind_uint64(read.get(), 4, token);
            if (sqlite::step(read.get()) != SQLITE_ROW)
                return false;
            const auto tenant = sqlite::column_text(read.get(), 0);
            sqlite::Statement pending(db, "UPDATE durable_queue SET state=CASE WHEN attempts>=max_attempts THEN ? ELSE ? END,owner='',lease_expires_at_ms=0,available_at_ms=? WHERE task_id=? AND state=? AND owner=? AND fencing_token=?");
            sqlite::bind_int(pending.get(), 1, state_value(QueueState::DeadLetter));
            sqlite::bind_int(pending.get(), 2, state_value(QueueState::Pending));
            sqlite::bind_int64(pending.get(), 3, available);
            sqlite::bind_text(pending.get(), 4, id);
            sqlite::bind_int(pending.get(), 5, state_value(QueueState::Leased));
            sqlite::bind_text(pending.get(), 6, worker);
            sqlite::bind_uint64(pending.get(), 7, token);
            if (sqlite::step(pending.get()) != SQLITE_DONE || sqlite::changes(db) != 1)
                throw std::runtime_error("queue nack conflict");
            sqlite::Statement release(db, "UPDATE tenant_quota SET active=active-1 WHERE tenant_id=? AND active>0");
            sqlite::bind_text(release.get(), 1, tenant);
            if (sqlite::step(release.get()) != SQLITE_DONE || sqlite::changes(db) != 1)
                throw std::runtime_error("quota release failed");
            tx.commit();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
    bool SQLiteDurableQueue::nack_with_quota_at(std::string_view id, std::string_view worker,
                                                 std::uint64_t token, std::int64_t now,
                                                 std::int64_t available)
    {
        if (now < 0 || available < now)
            return false;
        std::lock_guard l(mutex_);
        try
        {
            auto *db = sqlite::database(db_);
            sqlite::Transaction tx(db);
            sqlite::Statement read(db, "SELECT tenant_id FROM durable_queue WHERE task_id=? AND state=? AND owner=? AND fencing_token=? AND lease_expires_at_ms>?");
            sqlite::bind_text(read.get(), 1, id);
            sqlite::bind_int(read.get(), 2, state_value(QueueState::Leased));
            sqlite::bind_text(read.get(), 3, worker);
            sqlite::bind_uint64(read.get(), 4, token);
            sqlite::bind_int64(read.get(), 5, now);
            if (sqlite::step(read.get()) != SQLITE_ROW)
                return false;
            const auto tenant = sqlite::column_text(read.get(), 0);
            sqlite::Statement pending(db, "UPDATE durable_queue SET state=CASE WHEN attempts>=max_attempts THEN ? ELSE ? END,owner='',lease_expires_at_ms=0,available_at_ms=? WHERE task_id=? AND state=? AND owner=? AND fencing_token=? AND lease_expires_at_ms>?");
            sqlite::bind_int(pending.get(), 1, state_value(QueueState::DeadLetter));
            sqlite::bind_int(pending.get(), 2, state_value(QueueState::Pending));
            sqlite::bind_int64(pending.get(), 3, available);
            sqlite::bind_text(pending.get(), 4, id);
            sqlite::bind_int(pending.get(), 5, state_value(QueueState::Leased));
            sqlite::bind_text(pending.get(), 6, worker);
            sqlite::bind_uint64(pending.get(), 7, token);
            sqlite::bind_int64(pending.get(), 8, now);
            if (sqlite::step(pending.get()) != SQLITE_DONE || sqlite::changes(db) != 1)
                throw std::runtime_error("queue nack conflict");
            sqlite::Statement release(db, "UPDATE tenant_quota SET active=active-1 WHERE tenant_id=? AND active>0");
            sqlite::bind_text(release.get(), 1, tenant);
            if (sqlite::step(release.get()) != SQLITE_DONE || sqlite::changes(db) != 1)
                throw std::runtime_error("quota release failed");
            tx.commit();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
    std::optional<QueueTask> SQLiteDurableQueue::inspect(std::string_view id) const
    {
        std::lock_guard l(mutex_);
        auto *db = sqlite::database(db_);
        const std::string sql = std::string("SELECT ") + columns + " FROM durable_queue WHERE task_id=?";
        sqlite::Statement s(db, sql.c_str());
        sqlite::bind_text(s.get(), 1, id);
        return sqlite::step(s.get()) == SQLITE_ROW ? std::optional<QueueTask>(row(s.get())) : std::nullopt;
    }
} // namespace agent_framework::distributed
