#include "agent/distributed/postgres_durable_queue.hpp"

#include "agent/internal/postgres_utils.hpp"

#include <limits>
#include <stdexcept>
#include <vector>

namespace agent_framework::distributed {
namespace pg = agent_framework::internal::postgres;
namespace {
constexpr auto server_ms = "(extract(epoch from clock_timestamp())*1000)::bigint";

std::string number(std::uint64_t value) {
    if(value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::overflow_error("PostgreSQL integer exceeds BIGINT range");
    return std::to_string(value);
}
std::string number(std::int64_t value) { return std::to_string(value); }

QueueTask task_row(const pg::Result& row, int index = 0) {
    QueueTask task;
    task.task_id = row.value(index, 0);
    task.tenant_id = row.value(index, 1);
    task.idempotency_key = row.value(index, 2);
    task.payload_digest = row.value(index, 3);
    task.priority = static_cast<int>(pg::int64(row.value(index, 4)));
    task.available_at_ms = pg::int64(row.value(index, 5));
    task.attempts = static_cast<std::uint32_t>(pg::uint64(row.value(index, 6)));
    task.max_attempts = static_cast<std::uint32_t>(pg::uint64(row.value(index, 7)));
    task.state = static_cast<QueueState>(pg::uint64(row.value(index, 8)));
    task.owner = row.value(index, 9);
    task.fencing_token = pg::uint64(row.value(index, 10));
    task.lease_expires_at_ms = pg::int64(row.value(index, 11));
    return task;
}

bool invalid_identity(const QueueTask& task) {
    return task.task_id.empty() || task.tenant_id.empty() ||
           task.idempotency_key.empty() || task.payload_digest.empty() ||
           task.max_attempts == 0;
}
}  // namespace

PostgresDurableQueue::PostgresDurableQueue(std::string conninfo) {
    if(conninfo.empty()) throw std::invalid_argument("PostgreSQL conninfo required");
    connection_ = new pg::Connection(conninfo);
    migrate();
}
PostgresDurableQueue::~PostgresDurableQueue() {
    delete static_cast<pg::Connection*>(connection_);
}

void PostgresDurableQueue::migrate() {
    auto* connection = static_cast<pg::Connection*>(connection_)->get();
    pg::exec(connection,
        "CREATE TABLE IF NOT EXISTS agent_distributed_queue("
        "task_id text PRIMARY KEY,tenant_id text NOT NULL,idempotency_key text NOT NULL,"
        "payload_digest text NOT NULL,priority integer NOT NULL,available_at_ms bigint NOT NULL,"
        "attempts bigint NOT NULL,max_attempts bigint NOT NULL,state integer NOT NULL,"
        "owner text NOT NULL,fencing_token bigint NOT NULL,lease_expires_at_ms bigint NOT NULL,"
        "UNIQUE(tenant_id,idempotency_key),CHECK(attempts>=0 AND attempts<=max_attempts),"
        "CHECK(max_attempts>0),CHECK(state BETWEEN 0 AND 3));"
        "CREATE INDEX IF NOT EXISTS agent_queue_claim_idx ON agent_distributed_queue"
        "(tenant_id,state,priority DESC,available_at_ms,task_id);"
        "CREATE TABLE IF NOT EXISTS agent_tenant_quota("
        "tenant_id text PRIMARY KEY,maximum_active bigint NOT NULL CHECK(maximum_active>0),"
        "active bigint NOT NULL CHECK(active>=0 AND active<=maximum_active));");
}

bool PostgresDurableQueue::enqueue(QueueTask task, std::string* error) {
    if(invalid_identity(task) || task.attempts != 0 || task.fencing_token != 0 ||
       task.state != QueueState::Pending) {
        if(error) *error = "invalid queue task";
        return false;
    }
    std::lock_guard lock(mutex_);
    try {
        auto* connection = static_cast<pg::Connection*>(connection_)->get();
        pg::Transaction transaction(connection);
        const std::string sql = std::string(
            "INSERT INTO agent_distributed_queue VALUES($1,$2,$3,$4,$5,"
            "CASE WHEN $6::bigint<=0 THEN ") + server_ms +
            " ELSE $6::bigint END,0,$7,0,'',0,0) ON CONFLICT DO NOTHING";
        pg::exec_params(connection, sql,
            {task.task_id, task.tenant_id, task.idempotency_key, task.payload_digest,
             std::to_string(task.priority), number(task.available_at_ms),
             number(static_cast<std::uint64_t>(task.max_attempts))});
        auto existing = pg::exec_params(connection,
            "SELECT task_id,payload_digest FROM agent_distributed_queue "
            "WHERE tenant_id=$1 AND idempotency_key=$2",
            {task.tenant_id, task.idempotency_key});
        if(existing.rows() != 1 || existing.value(0, 0) != task.task_id ||
           existing.value(0, 1) != task.payload_digest) {
            if(error) *error = "queue idempotency conflict";
            return false;
        }
        transaction.commit();
        return true;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}

std::optional<Lease> PostgresDurableQueue::claim(std::string_view worker,
        std::string_view tenant, std::int64_t lease_ms, std::string* error) {
    return claim_impl(worker, tenant, lease_ms, false, error);
}
std::optional<Lease> PostgresDurableQueue::claim_with_quota(std::string_view worker,
        std::string_view tenant, std::int64_t lease_ms, std::string* error) {
    return claim_impl(worker, tenant, lease_ms, true, error);
}

std::optional<Lease> PostgresDurableQueue::claim_impl(std::string_view worker,
        std::string_view tenant, std::int64_t lease_ms, bool with_quota,
        std::string* error) {
    if(worker.empty() || tenant.empty() || lease_ms <= 0) return std::nullopt;
    std::lock_guard lock(mutex_);
    try {
        auto* connection = static_cast<pg::Connection*>(connection_)->get();
        pg::Transaction transaction(connection);
        std::uint64_t active = 0, maximum = 0;
        if(with_quota) {
            auto quota = pg::exec_params(connection,
                "SELECT maximum_active,active FROM agent_tenant_quota "
                "WHERE tenant_id=$1 FOR UPDATE", {std::string(tenant)});
            if(quota.rows() != 1) {
                if(error) *error = "tenant quota missing";
                return std::nullopt;
            }
            maximum = pg::uint64(quota.value(0, 0));
            active = pg::uint64(quota.value(0, 1));
            const std::string expire_sql = std::string(
                "UPDATE agent_distributed_queue SET state=3,owner='',lease_expires_at_ms=0 "
                "WHERE tenant_id=$1 AND state=1 AND lease_expires_at_ms<=") + server_ms +
                " AND attempts>=max_attempts RETURNING task_id";
            const auto expired = pg::exec_params(connection, expire_sql, {std::string(tenant)});
            if(expired.rows() > 0) {
                const auto released = static_cast<std::uint64_t>(expired.rows());
                active = released >= active ? 0 : active - released;
                pg::exec_params(connection,
                    "UPDATE agent_tenant_quota SET active=$2 WHERE tenant_id=$1",
                    {std::string(tenant), number(active)});
            }
        }
        const std::string select_sql = std::string(
            "SELECT task_id,tenant_id,idempotency_key,payload_digest,priority,available_at_ms,"
            "attempts,max_attempts,state,owner,fencing_token,lease_expires_at_ms "
            "FROM agent_distributed_queue WHERE tenant_id=$1 AND attempts<max_attempts AND "
            "((state=0 AND available_at_ms<=") + server_ms + ") OR (state=1 AND lease_expires_at_ms<=" +
            server_ms + ")) ORDER BY priority DESC,available_at_ms,task_id "
            "FOR UPDATE SKIP LOCKED LIMIT 1";
        auto selected = pg::exec_params(connection, select_sql, {std::string(tenant)});
        if(selected.rows() != 1) return std::nullopt;
        auto task = task_row(selected);
        const bool takeover = task.state == QueueState::Leased;
        if(with_quota && !takeover && active >= maximum) return std::nullopt;
        const std::string update_sql = std::string(
            "UPDATE agent_distributed_queue SET state=1,owner=$2,attempts=attempts+1,"
            "fencing_token=fencing_token+1,lease_expires_at_ms=") + server_ms +
            "+$3::bigint WHERE task_id=$1 RETURNING task_id,tenant_id,idempotency_key,"
            "payload_digest,priority,available_at_ms,attempts,max_attempts,state,owner,"
            "fencing_token,lease_expires_at_ms";
        auto updated = pg::exec_params(connection, update_sql,
            {task.task_id, std::string(worker), number(lease_ms)});
        if(updated.rows() != 1) throw std::runtime_error("claimed task disappeared");
        task = task_row(updated);
        if(with_quota && !takeover)
            pg::exec_params(connection,
                "UPDATE agent_tenant_quota SET active=active+1 WHERE tenant_id=$1",
                {std::string(tenant)});
        transaction.commit();
        return Lease{task, task.fencing_token};
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return std::nullopt;
    }
}

bool PostgresDurableQueue::renew(std::string_view task, std::string_view worker,
        std::uint64_t token, std::int64_t lease_ms, std::string* error) {
    if(task.empty() || worker.empty() || token == 0 || lease_ms <= 0) return false;
    std::lock_guard lock(mutex_);
    try {
        auto* connection = static_cast<pg::Connection*>(connection_)->get();
        const std::string sql = std::string(
            "UPDATE agent_distributed_queue SET lease_expires_at_ms=") + server_ms +
            "+$4::bigint WHERE task_id=$1 AND owner=$2 AND fencing_token=$3 AND state=1 "
            "AND lease_expires_at_ms>" + server_ms + " RETURNING task_id";
        return pg::exec_params(connection, sql,
            {std::string(task), std::string(worker), number(token), number(lease_ms)}).rows() == 1;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}

bool PostgresDurableQueue::ack(std::string_view task, std::string_view worker,
        std::uint64_t token, std::string* error) {
    return finish_impl(task, worker, token, false, true, 0, error);
}
bool PostgresDurableQueue::ack_with_quota(std::string_view task, std::string_view worker,
        std::uint64_t token, std::string* error) {
    return finish_impl(task, worker, token, true, true, 0, error);
}
bool PostgresDurableQueue::nack(std::string_view task, std::string_view worker,
        std::uint64_t token, std::int64_t delay, std::string* error) {
    return finish_impl(task, worker, token, false, false, delay, error);
}
bool PostgresDurableQueue::nack_with_quota(std::string_view task, std::string_view worker,
        std::uint64_t token, std::int64_t delay, std::string* error) {
    return finish_impl(task, worker, token, true, false, delay, error);
}

bool PostgresDurableQueue::finish_impl(std::string_view task, std::string_view worker,
        std::uint64_t token, bool with_quota, bool acknowledge, std::int64_t delay,
        std::string* error) {
    if(task.empty() || worker.empty() || token == 0 || delay < 0) return false;
    std::lock_guard lock(mutex_);
    try {
        auto* connection = static_cast<pg::Connection*>(connection_)->get();
        pg::Transaction transaction(connection);
        auto owned = pg::exec_params(connection,
            "SELECT tenant_id,attempts,max_attempts FROM agent_distributed_queue "
            "WHERE task_id=$1 AND owner=$2 AND fencing_token=$3 AND state=1 FOR UPDATE",
            {std::string(task), std::string(worker), number(token)});
        if(owned.rows() != 1) return false;
        const auto tenant = std::string(owned.value(0, 0));
        const auto attempts = pg::uint64(owned.value(0, 1));
        const auto maximum = pg::uint64(owned.value(0, 2));
        if(with_quota) {
            auto locked = pg::exec_params(connection,
                "SELECT active FROM agent_tenant_quota WHERE tenant_id=$1 FOR UPDATE", {tenant});
            if(locked.rows() != 1 || pg::uint64(locked.value(0, 0)) == 0) return false;
        }
        const int next_state = acknowledge ? 2 : (attempts >= maximum ? 3 : 0);
        const std::string sql = std::string(
            "UPDATE agent_distributed_queue SET state=$4,owner='',lease_expires_at_ms=0,"
            "available_at_ms=CASE WHEN $4::integer=0 THEN ") + server_ms +
            "+$5::bigint ELSE available_at_ms END WHERE task_id=$1 AND owner=$2 "
            "AND fencing_token=$3 AND state=1 RETURNING task_id";
        if(pg::exec_params(connection, sql,
            {std::string(task), std::string(worker), number(token),
             std::to_string(next_state), number(delay)}).rows() != 1) return false;
        if(with_quota)
            pg::exec_params(connection,
                "UPDATE agent_tenant_quota SET active=active-1 "
                "WHERE tenant_id=$1 AND active>0", {tenant});
        transaction.commit();
        return true;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}

bool PostgresDurableQueue::set_quota(std::string_view tenant, std::uint64_t maximum,
        std::string* error) {
    if(tenant.empty() || maximum == 0) return false;
    std::lock_guard lock(mutex_);
    try {
        auto* connection = static_cast<pg::Connection*>(connection_)->get();
        return pg::exec_params(connection,
            "INSERT INTO agent_tenant_quota VALUES($1,$2,0) ON CONFLICT(tenant_id) "
            "DO UPDATE SET maximum_active=excluded.maximum_active "
            "WHERE agent_tenant_quota.active<=excluded.maximum_active RETURNING tenant_id",
            {std::string(tenant), number(maximum)}).rows() == 1;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}

std::optional<TenantQuota> PostgresDurableQueue::quota(std::string_view tenant,
        std::string* error) const {
    std::lock_guard lock(mutex_);
    try {
        auto* connection = static_cast<pg::Connection*>(connection_)->get();
        auto result = pg::exec_params(connection,
            "SELECT maximum_active,active FROM agent_tenant_quota WHERE tenant_id=$1",
            {std::string(tenant)});
        if(result.rows() != 1) return std::nullopt;
        return TenantQuota{std::string(tenant), pg::uint64(result.value(0, 0)),
                           pg::uint64(result.value(0, 1))};
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return std::nullopt;
    }
}

std::optional<QueueTask> PostgresDurableQueue::inspect(std::string_view task,
        std::string* error) const {
    std::lock_guard lock(mutex_);
    try {
        auto* connection = static_cast<pg::Connection*>(connection_)->get();
        auto result = pg::exec_params(connection,
            "SELECT task_id,tenant_id,idempotency_key,payload_digest,priority,available_at_ms,"
            "attempts,max_attempts,state,owner,fencing_token,lease_expires_at_ms "
            "FROM agent_distributed_queue WHERE task_id=$1", {std::string(task)});
        return result.rows() == 1 ? std::optional<QueueTask>(task_row(result)) : std::nullopt;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return std::nullopt;
    }
}

}  // namespace agent_framework::distributed
