#pragma once
#include "agent/conversation/types.hpp"
#include "agent/assurance/types.hpp"
#include "agent/harness/task_closure.hpp"
namespace agent_framework::conversation
{
    struct ProfileDecision
    {
        bool allowed{false};
        TaskExecutionProfile profile{TaskExecutionProfile::Conversation};
        std::string reason_code;
        bool requires_harness{false};
    };
    class TaskProfileRouter
    {
    public:
        static ProfileDecision route(TaskExecutionProfile requested, bool side_effect,
                                     bool production);
    };
    std::optional<harness::TaskClosureContract> closure_contract_from(
        const assurance::AcceptanceContract &, TaskExecutionProfile, std::string *error = nullptr);
    bool outcome_can_close_task(const ModelTurnOutcome &) noexcept;
}
