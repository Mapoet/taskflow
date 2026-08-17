#pragma once

#include <string>

#include "agent/conversation/task_registry.hpp"

namespace agent_framework::conversation {

// Select the durable Run identity for an interactive turn. Executable turns
// get a fresh default Run; control-only turns retain the active Run. A
// non-empty configured id is an explicit caller-owned idempotency key.
std::string select_task_run_id(std::string_view configured_run_id,
                               std::string_view active_run_id,
                               bool control_only,
                               std::string_view turn_id);

struct TaskOpenResult {
    bool ok{false};
    PersistentTask task;
    TaskInputIntent intent{TaskInputIntent::InitialRequest};
    bool created{false};
    bool control_only{false};
    std::string error;
};

struct TaskPlanBinding {
    ConversationIdentity identity;
    std::string task_id;
    std::string run_id;
    std::uint64_t requirement_revision{0};
    std::uint64_t plan_revision{0};
    std::string plan_digest;
    std::string task_contract_digest;
};

class TaskOrchestrator {
public:
    explicit TaskOrchestrator(TaskRegistry& registry) : registry_(registry) {}

    TaskOpenResult open_or_resume(const TurnRequest& request,
                                  TaskInputIntent intent);
    TaskMutationResult bind_plan(const TaskPlanBinding& binding,
                                 std::uint64_t expected_task_revision);

private:
    TaskRegistry& registry_;
};

}  // namespace agent_framework::conversation
