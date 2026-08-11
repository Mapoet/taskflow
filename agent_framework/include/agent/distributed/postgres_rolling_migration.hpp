#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "agent/distributed/schema_migration.hpp"

namespace agent_framework::distributed {

class PostgresRollingMigrationExecutor {
public:
    explicit PostgresRollingMigrationExecutor(std::string conninfo);
    ~PostgresRollingMigrationExecutor();
    PostgresRollingMigrationExecutor(const PostgresRollingMigrationExecutor&) = delete;
    PostgresRollingMigrationExecutor& operator=(const PostgresRollingMigrationExecutor&) = delete;

    bool initialize(std::string_view component, std::uint64_t version,
                    std::uint64_t minimum_reader, std::string* error = nullptr);
    std::optional<MigrationLease> begin(std::string_view component,
        std::uint64_t expected_version, std::string_view owner,
        std::int64_t lease_ms, std::string* error = nullptr);
    bool reader_compatible(std::string_view component, std::uint64_t reader_version,
                           std::string* error = nullptr) const;
    std::optional<ComponentSchema> inspect_schema(std::string_view component,
                                                   std::string* error = nullptr) const;
    bool prepare(const RollingMigrationPlan&, std::string_view owner,
                 std::uint64_t fencing_token, std::string* error = nullptr);
    bool expand(const RollingMigrationPlan&, std::string_view owner,
                std::uint64_t fencing_token, std::string* error = nullptr);
    bool backfill(const RollingMigrationPlan&, std::string_view owner,
                  std::uint64_t fencing_token, std::string* error = nullptr);
    bool contract(const RollingMigrationPlan&, std::string_view owner,
                  std::uint64_t fencing_token, std::string* error = nullptr);
    std::optional<RollingMigrationState> inspect(std::string_view component,
                                                 std::string* error = nullptr) const;
private:
    void migrate();
    bool execute_phase(const RollingMigrationPlan&, std::string_view,
        std::uint64_t, RollingMigrationPhase, RollingMigrationPhase,
        std::string_view, bool, std::string*);
    void* connection_{nullptr};
    mutable std::mutex mutex_;
};

}  // namespace agent_framework::distributed
