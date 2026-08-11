#include "agent/distributed/schema_migration.hpp"

#include "agent/internal/sqlite_utils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

namespace agent_framework::distributed {
namespace {
namespace sqlite = agent_framework::internal::sqlite;
int phase_value(RollingMigrationPhase value) { return static_cast<int>(value); }
bool valid_plan(const RollingMigrationPlan& plan, std::string* error) {
    if(plan.component.empty() || plan.from_version == 0 || plan.to_version <= plan.from_version ||
       plan.minimum_reader_after_contract == 0 ||
       plan.minimum_reader_after_contract > plan.to_version || plan.plan_digest.empty() ||
       plan.expand_sql.empty() || plan.backfill_sql.empty() || plan.contract_sql.empty()) {
        if(error) *error = "invalid rolling migration plan";
        return false;
    }
    constexpr std::size_t maximum_sql = 1024 * 1024;
    for(const auto* sql : {&plan.expand_sql, &plan.backfill_sql, &plan.contract_sql}) {
        if(sql->size() > maximum_sql) { if(error) *error = "migration SQL exceeds limit"; return false; }
        std::string upper(*sql);
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        for(const auto* forbidden : {"BEGIN", "COMMIT", "ROLLBACK", "ATTACH", "DETACH",
                                     "PRAGMA", "VACUUM"}) {
            auto position = upper.find(forbidden);
            while(position != std::string::npos) {
                const auto is_identifier = [](char c) {
                    const auto value = static_cast<unsigned char>(c);
                    return std::isalnum(value) != 0 || c == '_';
                };
                const auto end = position + std::char_traits<char>::length(forbidden);
                const bool left_boundary = position == 0 || !is_identifier(upper[position - 1]);
                const bool right_boundary = end == upper.size() || !is_identifier(upper[end]);
                if(left_boundary && right_boundary) {
                if(error) *error = "migration SQL contains forbidden transaction/control statement";
                return false;
                }
                position = upper.find(forbidden, position + 1);
            }
        }
    }
    return true;
}
RollingMigrationState state_row(sqlite3_stmt* value) {
    return {sqlite::column_text(value, 0), sqlite::column_text(value, 1),
            static_cast<RollingMigrationPhase>(sqlite::column_int(value, 2)),
            sqlite::column_uint64(value, 3), sqlite::column_text(value, 4),
            sqlite::column_text(value, 5)};
}
}  // namespace

SQLiteRollingMigrationExecutor::SQLiteRollingMigrationExecutor(std::string path, int timeout) {
    if(path.empty()) throw std::invalid_argument("migration database path required");
    std::error_code filesystem_error;
    std::filesystem::path value(path);
    if(value.has_parent_path()) std::filesystem::create_directories(value.parent_path(), filesystem_error);
    sqlite3* opened = nullptr;
    if(filesystem_error || sqlite3_open_v2(path.c_str(), &opened,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const auto message = filesystem_error ? filesystem_error.message() :
            (opened ? sqlite3_errmsg(opened) : "sqlite open failed");
        if(opened) sqlite3_close(opened);
        throw std::runtime_error(message);
    }
    db_ = opened;
    sqlite3_busy_timeout(opened, timeout);
    migrate();
}
SQLiteRollingMigrationExecutor::~SQLiteRollingMigrationExecutor() {
    if(db_) sqlite3_close(sqlite::database(db_));
}
void SQLiteRollingMigrationExecutor::migrate() {
    auto* database = sqlite::database(db_);
    sqlite::exec(database, "PRAGMA journal_mode=WAL");
    sqlite::exec(database, "PRAGMA synchronous=FULL");
    sqlite::exec(database,
        "CREATE TABLE IF NOT EXISTS rolling_migration_execution("
        "component TEXT PRIMARY KEY,plan_digest TEXT NOT NULL,from_version INTEGER NOT NULL,"
        "to_version INTEGER NOT NULL,minimum_reader INTEGER NOT NULL,expand_sql TEXT NOT NULL,"
        "backfill_sql TEXT NOT NULL,contract_sql TEXT NOT NULL,phase INTEGER NOT NULL,"
        "fencing_token INTEGER NOT NULL,owner TEXT NOT NULL,last_error TEXT NOT NULL)");
}
bool SQLiteRollingMigrationExecutor::prepare(const RollingMigrationPlan& plan,
        std::string_view owner, std::uint64_t token, std::int64_t now, std::string* error) {
    if(!valid_plan(plan, error) || owner.empty() || token == 0 || now < 0) return false;
    std::lock_guard lock(mutex_);
    try {
        auto* database = sqlite::database(db_);
        sqlite::Transaction transaction(database);
        sqlite::Statement lease(database,
            "SELECT 1 FROM component_schema WHERE component=? AND version=? AND migration_owner=? "
            "AND migration_fencing_token=? AND migration_expires_at_ms>?");
        sqlite::bind_text(lease.get(), 1, plan.component);
        sqlite::bind_uint64(lease.get(), 2, plan.from_version);
        sqlite::bind_text(lease.get(), 3, owner);
        sqlite::bind_uint64(lease.get(), 4, token);
        sqlite::bind_int64(lease.get(), 5, now);
        if(sqlite::step(lease.get()) != SQLITE_ROW) {
            if(error) *error = "migration lease is stale or schema version changed";
            return false;
        }
        sqlite::Statement insert(database,
            "INSERT OR IGNORE INTO rolling_migration_execution VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
        sqlite::bind_text(insert.get(), 1, plan.component);
        sqlite::bind_text(insert.get(), 2, plan.plan_digest);
        sqlite::bind_uint64(insert.get(), 3, plan.from_version);
        sqlite::bind_uint64(insert.get(), 4, plan.to_version);
        sqlite::bind_uint64(insert.get(), 5, plan.minimum_reader_after_contract);
        sqlite::bind_text(insert.get(), 6, plan.expand_sql);
        sqlite::bind_text(insert.get(), 7, plan.backfill_sql);
        sqlite::bind_text(insert.get(), 8, plan.contract_sql);
        sqlite::bind_int(insert.get(), 9, phase_value(RollingMigrationPhase::Prepared));
        sqlite::bind_uint64(insert.get(), 10, token);
        sqlite::bind_text(insert.get(), 11, owner);
        sqlite::bind_text(insert.get(), 12, "");
        if(sqlite::step(insert.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(database));
        sqlite::Statement adopt(database,
            "UPDATE rolling_migration_execution SET fencing_token=?,owner=? WHERE component=? "
            "AND plan_digest=? AND from_version=? AND to_version=? AND minimum_reader=? "
            "AND expand_sql=? AND backfill_sql=? AND contract_sql=? AND phase<>?");
        sqlite::bind_uint64(adopt.get(), 1, token);
        sqlite::bind_text(adopt.get(), 2, owner);
        sqlite::bind_text(adopt.get(), 3, plan.component);
        sqlite::bind_text(adopt.get(), 4, plan.plan_digest);
        sqlite::bind_uint64(adopt.get(), 5, plan.from_version);
        sqlite::bind_uint64(adopt.get(), 6, plan.to_version);
        sqlite::bind_uint64(adopt.get(), 7, plan.minimum_reader_after_contract);
        sqlite::bind_text(adopt.get(), 8, plan.expand_sql);
        sqlite::bind_text(adopt.get(), 9, plan.backfill_sql);
        sqlite::bind_text(adopt.get(), 10, plan.contract_sql);
        sqlite::bind_int(adopt.get(), 11, phase_value(RollingMigrationPhase::Completed));
        if(sqlite::step(adopt.get()) != SQLITE_DONE || sqlite::changes(database) != 1) {
            if(error) *error = "migration plan conflicts with durable execution";
            return false;
        }
        sqlite::Statement verify(database,
            "SELECT 1 FROM rolling_migration_execution WHERE component=? AND plan_digest=? "
            "AND from_version=? AND to_version=? AND minimum_reader=? AND expand_sql=? "
            "AND backfill_sql=? AND contract_sql=? AND fencing_token=? AND owner=?");
        sqlite::bind_text(verify.get(), 1, plan.component); sqlite::bind_text(verify.get(), 2, plan.plan_digest);
        sqlite::bind_uint64(verify.get(), 3, plan.from_version); sqlite::bind_uint64(verify.get(), 4, plan.to_version);
        sqlite::bind_uint64(verify.get(), 5, plan.minimum_reader_after_contract);
        sqlite::bind_text(verify.get(), 6, plan.expand_sql); sqlite::bind_text(verify.get(), 7, plan.backfill_sql);
        sqlite::bind_text(verify.get(), 8, plan.contract_sql); sqlite::bind_uint64(verify.get(), 9, token);
        sqlite::bind_text(verify.get(), 10, owner);
        if(sqlite::step(verify.get()) != SQLITE_ROW) {
            if(error) *error = "migration plan conflicts with durable execution";
            return false;
        }
        transaction.commit();
        return true;
    } catch(const std::exception& exception) { if(error) *error = exception.what(); return false; }
}
bool SQLiteRollingMigrationExecutor::execute_phase(const RollingMigrationPlan& plan,
        std::string_view owner, std::uint64_t token, std::int64_t now,
        RollingMigrationPhase expected, RollingMigrationPhase next, std::string_view sql,
        bool terminal, std::string* error) {
    if(!valid_plan(plan, error) || owner.empty() || token == 0 || now < 0) return false;
    std::lock_guard lock(mutex_);
    try {
        auto* database = sqlite::database(db_);
        sqlite::Transaction transaction(database);
        RollingMigrationPhase phase;
        {
            sqlite::Statement current(database,
                "SELECT plan_digest,phase,fencing_token,owner,expand_sql,backfill_sql,contract_sql "
                "FROM rolling_migration_execution WHERE component=?");
            sqlite::bind_text(current.get(), 1, plan.component);
            if(sqlite::step(current.get()) != SQLITE_ROW ||
               sqlite::column_text(current.get(), 0) != plan.plan_digest ||
               sqlite::column_uint64(current.get(), 2) != token ||
               sqlite::column_text(current.get(), 3) != owner ||
               sqlite::column_text(current.get(), 4) != plan.expand_sql ||
               sqlite::column_text(current.get(), 5) != plan.backfill_sql ||
               sqlite::column_text(current.get(), 6) != plan.contract_sql) {
                if(error) *error = "migration execution identity mismatch";
                return false;
            }
            phase = static_cast<RollingMigrationPhase>(sqlite::column_int(current.get(), 1));
        }
        if(phase == next) { transaction.commit(); return true; }
        if(phase != expected) { if(error) *error = "migration phase transition rejected"; return false; }
        {
            sqlite::Statement lease(database,
                "SELECT 1 FROM component_schema WHERE component=? AND version=? AND migration_owner=? "
                "AND migration_fencing_token=? AND migration_expires_at_ms>?");
            sqlite::bind_text(lease.get(), 1, plan.component);
            sqlite::bind_uint64(lease.get(), 2, plan.from_version);
            sqlite::bind_text(lease.get(), 3, owner);
            sqlite::bind_uint64(lease.get(), 4, token);
            sqlite::bind_int64(lease.get(), 5, now);
            if(sqlite::step(lease.get()) != SQLITE_ROW) {
                if(error) *error = "migration lease expired or fenced";
                return false;
            }
        }
        sqlite::exec(database, std::string(sql).c_str());
        if(terminal) {
            sqlite::Statement schema(database,
                "UPDATE component_schema SET version=?,minimum_reader_version=?,migration_owner='',"
                "migration_expires_at_ms=0 WHERE component=? AND version=? AND migration_owner=? "
                "AND migration_fencing_token=? AND migration_expires_at_ms>?");
            sqlite::bind_uint64(schema.get(), 1, plan.to_version);
            sqlite::bind_uint64(schema.get(), 2, plan.minimum_reader_after_contract);
            sqlite::bind_text(schema.get(), 3, plan.component); sqlite::bind_uint64(schema.get(), 4, plan.from_version);
            sqlite::bind_text(schema.get(), 5, owner); sqlite::bind_uint64(schema.get(), 6, token);
            sqlite::bind_int64(schema.get(), 7, now);
            if(sqlite::step(schema.get()) != SQLITE_DONE || sqlite::changes(database) != 1)
                throw std::runtime_error("migration terminal fence conflict");
        }
        sqlite::Statement advance(database,
            "UPDATE rolling_migration_execution SET phase=?,last_error='' WHERE component=? "
            "AND plan_digest=? AND phase=? AND fencing_token=? AND owner=?");
        sqlite::bind_int(advance.get(), 1, phase_value(next)); sqlite::bind_text(advance.get(), 2, plan.component);
        sqlite::bind_text(advance.get(), 3, plan.plan_digest); sqlite::bind_int(advance.get(), 4, phase_value(expected));
        sqlite::bind_uint64(advance.get(), 5, token); sqlite::bind_text(advance.get(), 6, owner);
        if(sqlite::step(advance.get()) != SQLITE_DONE || sqlite::changes(database) != 1)
            throw std::runtime_error("migration phase fence conflict");
        transaction.commit();
        return true;
    } catch(const std::exception& exception) { if(error) *error = exception.what(); return false; }
}
bool SQLiteRollingMigrationExecutor::expand(const RollingMigrationPlan&p,std::string_view o,std::uint64_t t,std::int64_t n,std::string*e){return execute_phase(p,o,t,n,RollingMigrationPhase::Prepared,RollingMigrationPhase::Expanded,p.expand_sql,false,e);}
bool SQLiteRollingMigrationExecutor::backfill(const RollingMigrationPlan&p,std::string_view o,std::uint64_t t,std::int64_t n,std::string*e){return execute_phase(p,o,t,n,RollingMigrationPhase::Expanded,RollingMigrationPhase::Backfilled,p.backfill_sql,false,e);}
bool SQLiteRollingMigrationExecutor::contract(const RollingMigrationPlan&p,std::string_view o,std::uint64_t t,std::int64_t n,std::string*e){return execute_phase(p,o,t,n,RollingMigrationPhase::Backfilled,RollingMigrationPhase::Completed,p.contract_sql,true,e);}
std::optional<RollingMigrationState> SQLiteRollingMigrationExecutor::inspect(std::string_view component) const {
    std::lock_guard lock(mutex_); auto* database=sqlite::database(db_);
    sqlite::Statement statement(database,"SELECT component,plan_digest,phase,fencing_token,owner,last_error FROM rolling_migration_execution WHERE component=?");
    sqlite::bind_text(statement.get(),1,component);
    return sqlite::step(statement.get())==SQLITE_ROW?std::optional<RollingMigrationState>(state_row(statement.get())):std::nullopt;
}
}  // namespace agent_framework::distributed
