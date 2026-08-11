#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace agent_framework::distributed {

struct ComponentSchema {
    std::string component;
    std::uint64_t version{0};
    std::uint64_t minimum_reader_version{0};
    std::uint64_t migration_fencing_token{0};
    std::string migration_owner;
    std::int64_t migration_expires_at_ms{0};
};
struct MigrationLease {
    ComponentSchema schema;
    std::uint64_t fencing_token{0};
};

class SQLiteSchemaMigrationCoordinator {
public:
    explicit SQLiteSchemaMigrationCoordinator(std::string path, int busy_timeout_ms = 3000);
    ~SQLiteSchemaMigrationCoordinator();
    SQLiteSchemaMigrationCoordinator(const SQLiteSchemaMigrationCoordinator&) = delete;
    SQLiteSchemaMigrationCoordinator& operator=(const SQLiteSchemaMigrationCoordinator&) = delete;
    bool initialize(std::string_view component, std::uint64_t version,
                    std::uint64_t minimum_reader_version, std::string* error = nullptr);
    std::optional<ComponentSchema> inspect(std::string_view component) const;
    bool reader_compatible(std::string_view component,
                           std::uint64_t reader_version,
                           std::string* error = nullptr) const;
    std::optional<MigrationLease> begin(std::string_view component,
                                        std::uint64_t expected_version,
                                        std::string_view owner,
                                        std::int64_t now_ms,
                                        std::int64_t lease_ms,
                                        std::string* error = nullptr);
    bool renew(std::string_view component, std::string_view owner,
               std::uint64_t fencing_token, std::int64_t now_ms,
               std::int64_t lease_ms);
    bool commit(std::string_view component, std::string_view owner,
                std::uint64_t fencing_token, std::uint64_t new_version,
                std::uint64_t minimum_reader_version,
                std::string* error = nullptr);
private:
    void migrate();
    void* db_{nullptr};
    mutable std::mutex mutex_;
};

enum class RollingMigrationPhase { Prepared, Expanded, Backfilled, Completed, Failed };

struct RollingMigrationPlan {
    std::string component;
    std::uint64_t from_version{0};
    std::uint64_t to_version{0};
    std::uint64_t minimum_reader_after_contract{0};
    std::string plan_digest;
    std::string expand_sql;
    std::string backfill_sql;
    std::string contract_sql;
};

struct RollingMigrationState {
    std::string component;
    std::string plan_digest;
    RollingMigrationPhase phase{RollingMigrationPhase::Prepared};
    std::uint64_t fencing_token{0};
    std::string owner;
    std::string last_error;
};

class SQLiteRollingMigrationExecutor {
public:
    explicit SQLiteRollingMigrationExecutor(std::string path, int busy_timeout_ms = 3000);
    ~SQLiteRollingMigrationExecutor();
    SQLiteRollingMigrationExecutor(const SQLiteRollingMigrationExecutor&) = delete;
    SQLiteRollingMigrationExecutor& operator=(const SQLiteRollingMigrationExecutor&) = delete;
    bool prepare(const RollingMigrationPlan& plan, std::string_view owner,
                 std::uint64_t fencing_token, std::int64_t now_ms,
                 std::string* error = nullptr);
    bool expand(const RollingMigrationPlan& plan, std::string_view owner,
                std::uint64_t fencing_token, std::int64_t now_ms,
                std::string* error = nullptr);
    bool backfill(const RollingMigrationPlan& plan, std::string_view owner,
                  std::uint64_t fencing_token, std::int64_t now_ms,
                  std::string* error = nullptr);
    bool contract(const RollingMigrationPlan& plan, std::string_view owner,
                  std::uint64_t fencing_token, std::int64_t now_ms,
                  std::string* error = nullptr);
    std::optional<RollingMigrationState> inspect(std::string_view component) const;
private:
    void migrate();
    bool execute_phase(const RollingMigrationPlan&, std::string_view owner,
                       std::uint64_t token, std::int64_t now_ms,
                       RollingMigrationPhase expected, RollingMigrationPhase next,
                       std::string_view sql, bool terminal, std::string* error);
    void* db_{nullptr};
    mutable std::mutex mutex_;
};

}  // namespace agent_framework::distributed
