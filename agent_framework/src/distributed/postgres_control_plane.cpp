#include "agent/distributed/postgres_control_plane.hpp"

#include "agent/internal/postgres_utils.hpp"

#include <limits>
#include <stdexcept>

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
}  // namespace

PostgresWorkerRegistry::PostgresWorkerRegistry(std::string conninfo) {
    if(conninfo.empty()) throw std::invalid_argument("PostgreSQL conninfo required");
    connection_ = new pg::Connection(conninfo);
    migrate();
}
PostgresWorkerRegistry::~PostgresWorkerRegistry() {
    delete static_cast<pg::Connection*>(connection_);
}
void PostgresWorkerRegistry::migrate() {
    pg::exec(static_cast<pg::Connection*>(connection_)->get(),
        "CREATE TABLE IF NOT EXISTS agent_worker_registry("
        "worker_id text PRIMARY KEY,instance_id text NOT NULL,"
        "capabilities_digest text NOT NULL,generation bigint NOT NULL CHECK(generation>0),"
        "heartbeat_at_ms bigint NOT NULL)");
}
std::optional<std::uint64_t> PostgresWorkerRegistry::register_worker(
        std::string_view worker, std::string_view instance, std::string_view capabilities,
        std::string* error) {
    if(worker.empty() || instance.empty() || capabilities.empty()) return std::nullopt;
    std::lock_guard lock(mutex_);
    try {
        auto* connection = static_cast<pg::Connection*>(connection_)->get();
        const std::string sql = std::string(
            "INSERT INTO agent_worker_registry VALUES($1,$2,$3,1,") + server_ms +
            ") ON CONFLICT(worker_id) DO UPDATE SET instance_id=excluded.instance_id,"
            "capabilities_digest=excluded.capabilities_digest,generation="
            "agent_worker_registry.generation+1,heartbeat_at_ms=" + server_ms +
            " RETURNING generation";
        auto result = pg::exec_params(connection, sql,
            {std::string(worker), std::string(instance), std::string(capabilities)});
        return result.rows() == 1
            ? std::optional<std::uint64_t>(pg::uint64(result.value(0, 0))) : std::nullopt;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return std::nullopt;
    }
}
bool PostgresWorkerRegistry::heartbeat(std::string_view worker, std::string_view instance,
        std::uint64_t generation, std::string* error) {
    std::lock_guard lock(mutex_);
    try {
        auto* connection = static_cast<pg::Connection*>(connection_)->get();
        const std::string sql = std::string(
            "UPDATE agent_worker_registry SET heartbeat_at_ms=") + server_ms +
            " WHERE worker_id=$1 AND instance_id=$2 AND generation=$3 "
            "RETURNING worker_id";
        return pg::exec_params(connection, sql,
            {std::string(worker), std::string(instance), number(generation)}).rows() == 1;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}
std::vector<WorkerRecord> PostgresWorkerRegistry::alive(std::int64_t staleness,
        std::string* error) const {
    std::vector<WorkerRecord> output;
    if(staleness < 0) return output;
    std::lock_guard lock(mutex_);
    try {
        auto* connection = static_cast<pg::Connection*>(connection_)->get();
        const std::string sql = std::string(
            "SELECT worker_id,instance_id,capabilities_digest,generation,heartbeat_at_ms "
            "FROM agent_worker_registry WHERE heartbeat_at_ms>=") + server_ms +
            "-$1::bigint ORDER BY worker_id";
        auto result = pg::exec_params(connection, sql, {number(staleness)});
        for(int row = 0; row < result.rows(); ++row)
            output.push_back({std::string(result.value(row, 0)),
                std::string(result.value(row, 1)), std::string(result.value(row, 2)),
                pg::uint64(result.value(row, 3)), pg::int64(result.value(row, 4))});
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
    }
    return output;
}

PostgresLeaderElection::PostgresLeaderElection(std::string conninfo) {
    if(conninfo.empty()) throw std::invalid_argument("PostgreSQL conninfo required");
    connection_ = new pg::Connection(conninfo);
    migrate();
}
PostgresLeaderElection::~PostgresLeaderElection() {
    delete static_cast<pg::Connection*>(connection_);
}
void PostgresLeaderElection::migrate() {
    pg::exec(static_cast<pg::Connection*>(connection_)->get(),
        "CREATE TABLE IF NOT EXISTS agent_leader_election("
        "election text PRIMARY KEY,owner text NOT NULL,fencing_token bigint NOT NULL,"
        "expires_at_ms bigint NOT NULL)");
}
std::optional<LeaderLease> PostgresLeaderElection::acquire(std::string_view election,
        std::string_view owner, std::int64_t lease_ms, std::string* error) {
    if(election.empty() || owner.empty() || lease_ms <= 0) return std::nullopt;
    std::lock_guard lock(mutex_);
    try {
        auto* connection = static_cast<pg::Connection*>(connection_)->get();
        pg::Transaction transaction(connection);
        pg::exec_params(connection,
            "INSERT INTO agent_leader_election VALUES($1,'',0,0) ON CONFLICT DO NOTHING",
            {std::string(election)});
        const std::string sql = std::string(
            "UPDATE agent_leader_election SET owner=$2,fencing_token=fencing_token+1,"
            "expires_at_ms=") + server_ms + "+$3::bigint WHERE election=$1 AND "
            "(owner='' OR expires_at_ms<=" + server_ms +
            ") RETURNING fencing_token,expires_at_ms";
        auto result = pg::exec_params(connection, sql,
            {std::string(election), std::string(owner), number(lease_ms)});
        if(result.rows() != 1) return std::nullopt;
        LeaderLease lease{std::string(election), std::string(owner),
            pg::uint64(result.value(0, 0)), pg::int64(result.value(0, 1))};
        transaction.commit();
        return lease;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return std::nullopt;
    }
}
bool PostgresLeaderElection::renew(const LeaderLease& lease, std::int64_t lease_ms,
        std::string* error) {
    if(lease_ms <= 0) return false;
    std::lock_guard lock(mutex_);
    try {
        auto* connection = static_cast<pg::Connection*>(connection_)->get();
        const std::string sql = std::string(
            "UPDATE agent_leader_election SET expires_at_ms=") + server_ms +
            "+$4::bigint WHERE election=$1 AND owner=$2 AND fencing_token=$3 "
            "AND expires_at_ms>" + server_ms + " RETURNING election";
        return pg::exec_params(connection, sql,
            {lease.election, lease.owner, number(lease.fencing_token),
             number(lease_ms)}).rows() == 1;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}
bool PostgresLeaderElection::release(const LeaderLease& lease, std::string* error) {
    std::lock_guard lock(mutex_);
    try {
        auto* connection = static_cast<pg::Connection*>(connection_)->get();
        return pg::exec_params(connection,
            "UPDATE agent_leader_election SET owner='',expires_at_ms=0 WHERE election=$1 "
            "AND owner=$2 AND fencing_token=$3 RETURNING election",
            {lease.election, lease.owner, number(lease.fencing_token)}).rows() == 1;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}
bool PostgresLeaderElection::is_current(const LeaderLease& lease, std::string* error) const {
    std::lock_guard lock(mutex_);
    try {
        auto* connection = static_cast<pg::Connection*>(connection_)->get();
        const std::string sql = std::string(
            "SELECT 1 FROM agent_leader_election WHERE election=$1 AND owner=$2 "
            "AND fencing_token=$3 AND expires_at_ms>") + server_ms;
        return pg::exec_params(connection, sql,
            {lease.election, lease.owner, number(lease.fencing_token)}).rows() == 1;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}
std::optional<LeaderLease> PostgresLeaderElection::inspect(std::string_view election,
        std::string* error) const {
    std::lock_guard lock(mutex_);
    try {
        auto* connection = static_cast<pg::Connection*>(connection_)->get();
        auto result = pg::exec_params(connection,
            "SELECT owner,fencing_token,expires_at_ms FROM agent_leader_election "
            "WHERE election=$1", {std::string(election)});
        if(result.rows() != 1) return std::nullopt;
        return LeaderLease{std::string(election), std::string(result.value(0, 0)),
            pg::uint64(result.value(0, 1)), pg::int64(result.value(0, 2))};
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return std::nullopt;
    }
}

}  // namespace agent_framework::distributed
