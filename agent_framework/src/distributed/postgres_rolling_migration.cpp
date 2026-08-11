#include "agent/distributed/postgres_rolling_migration.hpp"

#include "agent/internal/postgres_utils.hpp"

#include <algorithm>
#include <cctype>
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
std::string phase(RollingMigrationPhase value) {
    return std::to_string(static_cast<int>(value));
}
bool valid_sql(std::string_view sql) {
    if(sql.empty() || sql.size() > 1024U * 1024U) return false;
    std::string upper(sql);
    std::transform(upper.begin(), upper.end(), upper.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const auto identifier = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    };
    for(const auto* token : {"BEGIN", "COMMIT", "ROLLBACK", "PREPARE", "COPY", "VACUUM"}) {
        auto position = upper.find(token);
        while(position != std::string::npos) {
            const auto end = position + std::char_traits<char>::length(token);
            if((position == 0 || !identifier(upper[position - 1])) &&
               (end == upper.size() || !identifier(upper[end]))) return false;
            position = upper.find(token, position + 1);
        }
    }
    return true;
}
bool valid_plan(const RollingMigrationPlan& plan, std::string* error) {
    const bool valid = !plan.component.empty() && plan.from_version > 0 &&
        plan.to_version > plan.from_version && plan.minimum_reader_after_contract > 0 &&
        plan.minimum_reader_after_contract <= plan.to_version && !plan.plan_digest.empty() &&
        valid_sql(plan.expand_sql) && valid_sql(plan.backfill_sql) && valid_sql(plan.contract_sql);
    if(!valid && error) *error = "invalid PostgreSQL rolling migration plan";
    return valid;
}
ComponentSchema schema_row(const pg::Result& row) {
    return {std::string(row.value(0, 0)), pg::uint64(row.value(0, 1)),
        pg::uint64(row.value(0, 2)), pg::uint64(row.value(0, 3)),
        std::string(row.value(0, 4)), pg::int64(row.value(0, 5))};
}
}  // namespace

PostgresRollingMigrationExecutor::PostgresRollingMigrationExecutor(std::string conninfo) {
    if(conninfo.empty()) throw std::invalid_argument("PostgreSQL conninfo required");
    connection_ = new pg::Connection(conninfo);
    migrate();
}
PostgresRollingMigrationExecutor::~PostgresRollingMigrationExecutor() {
    delete static_cast<pg::Connection*>(connection_);
}
void PostgresRollingMigrationExecutor::migrate() {
    pg::exec(static_cast<pg::Connection*>(connection_)->get(),
        "CREATE TABLE IF NOT EXISTS agent_component_schema("
        "component text PRIMARY KEY,version bigint NOT NULL,minimum_reader_version bigint NOT NULL,"
        "migration_fencing_token bigint NOT NULL,migration_owner text NOT NULL,"
        "migration_expires_at_ms bigint NOT NULL);"
        "CREATE TABLE IF NOT EXISTS agent_rolling_migration("
        "component text PRIMARY KEY,plan_digest text NOT NULL,from_version bigint NOT NULL,"
        "to_version bigint NOT NULL,minimum_reader bigint NOT NULL,expand_sql text NOT NULL,"
        "backfill_sql text NOT NULL,contract_sql text NOT NULL,phase integer NOT NULL,"
        "fencing_token bigint NOT NULL,owner text NOT NULL,last_error text NOT NULL)");
}
bool PostgresRollingMigrationExecutor::initialize(std::string_view component,
        std::uint64_t version, std::uint64_t minimum, std::string* error) {
    if(component.empty() || version == 0 || minimum == 0 || minimum > version) return false;
    std::lock_guard lock(mutex_);
    try {
        auto result = pg::exec_params(static_cast<pg::Connection*>(connection_)->get(),
            "INSERT INTO agent_component_schema VALUES($1,$2,$3,0,'',0) "
            "ON CONFLICT DO NOTHING RETURNING component",
            {std::string(component), number(version), number(minimum)});
        return result.rows() == 1;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}
std::optional<MigrationLease> PostgresRollingMigrationExecutor::begin(
        std::string_view component, std::uint64_t expected, std::string_view owner,
        std::int64_t lease_ms, std::string* error) {
    if(component.empty() || owner.empty() || lease_ms <= 0) return std::nullopt;
    std::lock_guard lock(mutex_);
    try {
        auto* connection = static_cast<pg::Connection*>(connection_)->get();
        const std::string sql = std::string(
            "UPDATE agent_component_schema SET migration_owner=$3,"
            "migration_fencing_token=migration_fencing_token+1,migration_expires_at_ms=") +
            server_ms + "+$4::bigint WHERE component=$1 AND version=$2 AND "
            "(migration_owner='' OR migration_expires_at_ms<=" + server_ms +
            ") RETURNING component,version,minimum_reader_version,migration_fencing_token,"
            "migration_owner,migration_expires_at_ms";
        auto result = pg::exec_params(connection, sql,
            {std::string(component), number(expected), std::string(owner), number(lease_ms)});
        if(result.rows() != 1) return std::nullopt;
        auto schema = schema_row(result);
        return MigrationLease{schema, schema.migration_fencing_token};
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return std::nullopt;
    }
}
std::optional<ComponentSchema> PostgresRollingMigrationExecutor::inspect_schema(
        std::string_view component, std::string* error) const {
    std::lock_guard lock(mutex_);
    try {
        auto result = pg::exec_params(static_cast<pg::Connection*>(connection_)->get(),
            "SELECT component,version,minimum_reader_version,migration_fencing_token,"
            "migration_owner,migration_expires_at_ms FROM agent_component_schema WHERE component=$1",
            {std::string(component)});
        return result.rows() == 1 ? std::optional<ComponentSchema>(schema_row(result)) : std::nullopt;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return std::nullopt;
    }
}
bool PostgresRollingMigrationExecutor::reader_compatible(std::string_view component,
        std::uint64_t reader, std::string* error) const {
    const auto schema = inspect_schema(component, error);
    if(!schema) return false;
    if(reader < schema->minimum_reader_version) {
        if(error) *error = "reader version is below schema compatibility floor";
        return false;
    }
    return true;
}
bool PostgresRollingMigrationExecutor::prepare(const RollingMigrationPlan& plan,
        std::string_view owner, std::uint64_t token, std::string* error) {
    if(!valid_plan(plan, error) || owner.empty() || token == 0) return false;
    std::lock_guard lock(mutex_);
    try {
        auto* connection = static_cast<pg::Connection*>(connection_)->get();
        pg::Transaction transaction(connection);
        const std::string lease_sql = std::string(
            "SELECT 1 FROM agent_component_schema WHERE component=$1 AND version=$2 "
            "AND migration_owner=$3 AND migration_fencing_token=$4 AND migration_expires_at_ms>") +
            server_ms + " FOR UPDATE";
        if(pg::exec_params(connection, lease_sql,
            {plan.component, number(plan.from_version), std::string(owner), number(token)}).rows() != 1) {
            if(error) *error = "PostgreSQL migration lease is stale";
            return false;
        }
        pg::exec_params(connection,
            "INSERT INTO agent_rolling_migration VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,'') "
            "ON CONFLICT DO NOTHING",
            {plan.component, plan.plan_digest, number(plan.from_version), number(plan.to_version),
             number(plan.minimum_reader_after_contract), plan.expand_sql, plan.backfill_sql,
             plan.contract_sql, phase(RollingMigrationPhase::Prepared), number(token),
             std::string(owner)});
        auto adopted = pg::exec_params(connection,
            "UPDATE agent_rolling_migration SET fencing_token=$10,owner=$11 WHERE component=$1 "
            "AND plan_digest=$2 AND from_version=$3 AND to_version=$4 AND minimum_reader=$5 "
            "AND expand_sql=$6 AND backfill_sql=$7 AND contract_sql=$8 AND phase<>$9 "
            "RETURNING component",
            {plan.component, plan.plan_digest, number(plan.from_version), number(plan.to_version),
             number(plan.minimum_reader_after_contract), plan.expand_sql, plan.backfill_sql,
             plan.contract_sql, phase(RollingMigrationPhase::Completed), number(token),
             std::string(owner)});
        if(adopted.rows() != 1) {
            if(error) *error = "PostgreSQL migration plan conflicts with durable execution";
            return false;
        }
        transaction.commit();
        return true;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}
bool PostgresRollingMigrationExecutor::execute_phase(const RollingMigrationPlan& plan,
        std::string_view owner, std::uint64_t token, RollingMigrationPhase expected,
        RollingMigrationPhase next, std::string_view sql, bool terminal, std::string* error) {
    if(!valid_plan(plan, error) || owner.empty() || token == 0) return false;
    std::lock_guard lock(mutex_);
    try {
        auto* connection = static_cast<pg::Connection*>(connection_)->get();
        pg::Transaction transaction(connection);
        auto execution = pg::exec_params(connection,
            "SELECT plan_digest,phase,fencing_token,owner,expand_sql,backfill_sql,contract_sql "
            "FROM agent_rolling_migration WHERE component=$1 FOR UPDATE", {plan.component});
        if(execution.rows() != 1 || execution.value(0, 0) != plan.plan_digest ||
           pg::uint64(execution.value(0, 2)) != token || execution.value(0, 3) != owner ||
           execution.value(0, 4) != plan.expand_sql || execution.value(0, 5) != plan.backfill_sql ||
           execution.value(0, 6) != plan.contract_sql) return false;
        const auto current = static_cast<RollingMigrationPhase>(pg::uint64(execution.value(0, 1)));
        if(current == next) { transaction.commit(); return true; }
        if(current != expected) return false;
        const std::string lease_sql = std::string(
            "SELECT 1 FROM agent_component_schema WHERE component=$1 AND version=$2 "
            "AND migration_owner=$3 AND migration_fencing_token=$4 AND migration_expires_at_ms>") +
            server_ms + " FOR UPDATE";
        if(pg::exec_params(connection, lease_sql,
            {plan.component, number(plan.from_version), std::string(owner), number(token)}).rows() != 1)
            return false;
        pg::exec(connection, sql);
        if(terminal) {
            const std::string schema_sql = std::string(
                "UPDATE agent_component_schema SET version=$2,minimum_reader_version=$3,"
                "migration_owner='',migration_expires_at_ms=0 WHERE component=$1 AND version=$4 "
                "AND migration_owner=$5 AND migration_fencing_token=$6 AND migration_expires_at_ms>") +
                server_ms + " RETURNING component";
            if(pg::exec_params(connection, schema_sql,
                {plan.component, number(plan.to_version), number(plan.minimum_reader_after_contract),
                 number(plan.from_version), std::string(owner), number(token)}).rows() != 1)
                throw std::runtime_error("PostgreSQL terminal migration fence conflict");
        }
        if(pg::exec_params(connection,
            "UPDATE agent_rolling_migration SET phase=$2,last_error='' WHERE component=$1 "
            "AND phase=$3 AND fencing_token=$4 AND owner=$5 RETURNING component",
            {plan.component, phase(next), phase(expected), number(token), std::string(owner)}).rows() != 1)
            throw std::runtime_error("PostgreSQL migration phase fence conflict");
        transaction.commit();
        return true;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}
bool PostgresRollingMigrationExecutor::expand(const RollingMigrationPlan& plan,
        std::string_view owner, std::uint64_t token, std::string* error) {
    return execute_phase(plan, owner, token, RollingMigrationPhase::Prepared,
        RollingMigrationPhase::Expanded, plan.expand_sql, false, error);
}
bool PostgresRollingMigrationExecutor::backfill(const RollingMigrationPlan& plan,
        std::string_view owner, std::uint64_t token, std::string* error) {
    return execute_phase(plan, owner, token, RollingMigrationPhase::Expanded,
        RollingMigrationPhase::Backfilled, plan.backfill_sql, false, error);
}
bool PostgresRollingMigrationExecutor::contract(const RollingMigrationPlan& plan,
        std::string_view owner, std::uint64_t token, std::string* error) {
    return execute_phase(plan, owner, token, RollingMigrationPhase::Backfilled,
        RollingMigrationPhase::Completed, plan.contract_sql, true, error);
}
std::optional<RollingMigrationState> PostgresRollingMigrationExecutor::inspect(
        std::string_view component, std::string* error) const {
    std::lock_guard lock(mutex_);
    try {
        auto result = pg::exec_params(static_cast<pg::Connection*>(connection_)->get(),
            "SELECT component,plan_digest,phase,fencing_token,owner,last_error "
            "FROM agent_rolling_migration WHERE component=$1", {std::string(component)});
        if(result.rows() != 1) return std::nullopt;
        return RollingMigrationState{std::string(result.value(0, 0)),
            std::string(result.value(0, 1)),
            static_cast<RollingMigrationPhase>(pg::uint64(result.value(0, 2))),
            pg::uint64(result.value(0, 3)), std::string(result.value(0, 4)),
            std::string(result.value(0, 5))};
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return std::nullopt;
    }
}

}  // namespace agent_framework::distributed
