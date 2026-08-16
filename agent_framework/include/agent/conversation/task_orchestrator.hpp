#pragma once

#include <string>

#include "agent/conversation/task_registry.hpp"

namespace agent_framework::conversation {

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
