#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>

#include <sqlite3.h>

#include "agent/distributed/schema_migration.hpp"
#include "agent/internal/sqlite_utils.hpp"

namespace {
namespace sqlite = agent_framework::internal::sqlite;

class Database {
public:
    explicit Database(const std::string& path) {
        assert(sqlite3_open_v2(path.c_str(), &value_, SQLITE_OPEN_READWRITE,
                               nullptr) == SQLITE_OK);
    }
    ~Database() { sqlite3_close(value_); }
    sqlite3* get() const { return value_; }
private:
    sqlite3* value_{nullptr};
};

bool has_column(sqlite3* database, std::string_view table, std::string_view column) {
    sqlite::Statement statement(database, "SELECT name FROM pragma_table_info(?) WHERE name=?");
    sqlite::bind_text(statement.get(), 1, table);
    sqlite::bind_text(statement.get(), 2, column);
    return sqlite::step(statement.get()) == SQLITE_ROW;
}

std::string scalar_text(sqlite3* database, const char* sql) {
    sqlite::Statement statement(database, sql);
    assert(sqlite::step(statement.get()) == SQLITE_ROW);
    return sqlite::column_text(statement.get(), 0);
}

void require(bool condition, const std::string& context, const std::string& error) {
    if(condition) return;
    std::cerr << context << ": " << error << '\n';
    std::abort();
}
}  // namespace

int main() {
    using namespace agent_framework::distributed;
    const auto path = (std::filesystem::temp_directory_path() /
                       "phase4-rolling-migration.sqlite").string();
    std::filesystem::remove(path);

    SQLiteSchemaMigrationCoordinator coordinator(path);
    std::string error;
    assert(coordinator.initialize("records", 1, 1, &error));
    {
        Database database(path);
        sqlite::exec(database.get(),
            "CREATE TABLE records(id INTEGER PRIMARY KEY,old_value TEXT NOT NULL);"
            "INSERT INTO records VALUES(1,'alpha'),(2,'beta');");
    }
    const auto lease = coordinator.begin("records", 1, "migrator-a", 1000, 100, &error);
    assert(lease && lease->fencing_token == 1);

    const RollingMigrationPlan plan{
        "records", 1, 2, 2, "sha256:rolling-records-v2",
        "ALTER TABLE records ADD COLUMN new_value TEXT;",
        "UPDATE records SET new_value=upper(old_value);",
        "CREATE TABLE records_next(id INTEGER PRIMARY KEY,new_value TEXT NOT NULL);"
        "INSERT INTO records_next SELECT id,new_value FROM records;"
        "DROP TABLE records;ALTER TABLE records_next RENAME TO records;"};

    SQLiteRollingMigrationExecutor executor(path);
    assert(executor.prepare(plan, "migrator-a", lease->fencing_token, 1001, &error));
    auto conflicting = plan;
    conflicting.expand_sql = "ALTER TABLE records ADD COLUMN conflict TEXT;";
    assert(!executor.prepare(conflicting, "migrator-a", lease->fencing_token, 1001, &error));
    assert(!executor.backfill(plan, "migrator-a", lease->fencing_token, 1002, &error));
    assert(executor.expand(plan, "migrator-a", lease->fencing_token, 1002, &error));
    assert(executor.expand(plan, "migrator-a", lease->fencing_token, 1002, &error));
    {
        Database database(path);
        assert(has_column(database.get(), "records", "old_value"));
        assert(has_column(database.get(), "records", "new_value"));
    }

    {
        SQLiteRollingMigrationExecutor restarted(path);
        auto state = restarted.inspect("records");
        assert(state && state->phase == RollingMigrationPhase::Expanded);
        assert(restarted.backfill(plan, "migrator-a", lease->fencing_token, 1003, &error));
        assert(restarted.backfill(plan, "migrator-a", lease->fencing_token, 1003, &error));
    }
    {
        Database database(path);
        assert(scalar_text(database.get(),
            "SELECT new_value FROM records WHERE id=1") == "ALPHA");
    }

    require(executor.contract(plan, "migrator-a", lease->fencing_token, 1004, &error),
            "contract failed", error);
    assert(executor.contract(plan, "migrator-a", lease->fencing_token, 1004, &error));
    {
        Database database(path);
        assert(!has_column(database.get(), "records", "old_value"));
        assert(has_column(database.get(), "records", "new_value"));
        assert(scalar_text(database.get(),
            "SELECT new_value FROM records WHERE id=2") == "BETA");
    }
    auto schema = coordinator.inspect("records");
    assert(schema && schema->version == 2 && schema->minimum_reader_version == 2);
    assert(schema->migration_owner.empty());
    assert(!coordinator.reader_compatible("records", 1, &error));
    assert(coordinator.reader_compatible("records", 2, &error));

    // Failed DDL rolls back both the application schema and phase marker.
    assert(coordinator.initialize("rollback_case", 1, 1, &error));
    {
        Database database(path);
        sqlite::exec(database.get(),
            "CREATE TABLE rollback_case(id INTEGER PRIMARY KEY,value TEXT NOT NULL);"
            "INSERT INTO rollback_case VALUES(1,'kept');");
    }
    const auto rollback_lease = coordinator.begin(
        "rollback_case", 1, "migrator-b", 2000, 100, &error);
    assert(rollback_lease);
    const RollingMigrationPlan failing{
        "rollback_case", 1, 2, 1, "sha256:rollback",
        "ALTER TABLE rollback_case ADD COLUMN temporary TEXT;"
        "INSERT INTO missing_table VALUES(1);",
        "UPDATE rollback_case SET temporary=value;",
        "SELECT 1;"};
    assert(executor.prepare(failing, "migrator-b", rollback_lease->fencing_token,
                            2001, &error));
    assert(!executor.expand(failing, "migrator-b", rollback_lease->fencing_token,
                            2002, &error));
    assert(executor.inspect("rollback_case")->phase == RollingMigrationPhase::Prepared);
    {
        Database database(path);
        assert(!has_column(database.get(), "rollback_case", "temporary"));
        assert(scalar_text(database.get(),
            "SELECT value FROM rollback_case WHERE id=1") == "kept");
    }

    // Lease expiry/takeover fences the old owner and token.
    const auto takeover = coordinator.begin(
        "rollback_case", 1, "migrator-c", 2100, 100, &error);
    assert(takeover && takeover->fencing_token == rollback_lease->fencing_token + 1);
    assert(!executor.expand(failing, "migrator-b", rollback_lease->fencing_token,
                            2101, &error));
    assert(executor.prepare(failing, "migrator-c", takeover->fencing_token, 2101, &error));
    const auto adopted = executor.inspect("rollback_case");
    assert(adopted && adopted->owner == "migrator-c" &&
           adopted->fencing_token == takeover->fencing_token);
    assert(!executor.expand(failing, "migrator-c", takeover->fencing_token,
                            2102, &error));

    auto forbidden = failing;
    forbidden.component = "forbidden";
    forbidden.plan_digest = "sha256:forbidden";
    forbidden.expand_sql = "BEGIN; SELECT 1; COMMIT;";
    assert(!executor.prepare(forbidden, "migrator-c", takeover->fencing_token,
                             2101, &error));
    assert(!coordinator.begin("rollback_case", 1, "overflow", 1,
        std::numeric_limits<std::int64_t>::max(), &error));

    {
        SQLiteRollingMigrationExecutor restarted(path);
        const auto state = restarted.inspect("records");
        assert(state && state->phase == RollingMigrationPhase::Completed);
    }
    std::filesystem::remove(path);
    return 0;
}
