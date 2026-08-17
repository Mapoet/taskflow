#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace agent_framework::conversation {

struct TaskExecutionSnapshot {
    std::string tenant_id,session_id,conversation_id,task_id,run_id,turn_id,plan_id;
    std::uint64_t task_revision{0},requirement_revision{0},plan_revision{0};
    std::uint64_t run_revision{0},turn_revision{0},projection_revision{0};
    std::string task_digest,requirement_digest,plan_digest,run_digest,projection_digest;
    std::string snapshot_digest;
};

struct SnapshotExpectation {
    std::string idempotency_key;
    std::optional<std::uint64_t> task_revision,requirement_revision,plan_revision;
    std::optional<std::uint64_t> run_revision,turn_revision,projection_revision;
};

struct SnapshotConflict {
    bool matches{false};
    bool safe_retry{false};
    std::vector<std::string> changed_fields;
    TaskExecutionSnapshot current;
};

nlohmann::json encode(const TaskExecutionSnapshot&);
std::vector<std::string> validate(const TaskExecutionSnapshot&);
SnapshotConflict compare(const TaskExecutionSnapshot&,const SnapshotExpectation&,
                         bool command_is_idempotent);

}  // namespace agent_framework::conversation
