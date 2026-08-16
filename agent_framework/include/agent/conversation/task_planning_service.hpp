#pragma once

#include <string>
#include <vector>

#include "agent/conversation/task_orchestrator.hpp"
#include "agent/conversation/context_projection.hpp"
#include "agent/assurance/types.hpp"
#include "agent/harness/sqlite_production_input_repository.hpp"
#include "agent/planning/cognition_pipeline.hpp"

namespace agent_framework::conversation {

enum class TaskPlanningState {
    Planned,
    AwaitingClarification,
    AwaitingApproval,
    Failed
};

struct TaskPlanningResult {
    TaskPlanningState state{TaskPlanningState::Failed};
    std::uint64_t task_revision{0};
    std::uint64_t plan_revision{0};
    std::string plan_digest;
    std::string task_contract_digest;
    std::string context_projection_digest;
    std::string acceptance_contract_digest;
    std::vector<std::string> clarification_questions;
    std::string error_code;
    std::string error_message;
};

struct TaskPlanningPolicy {
    std::vector<std::string> requested_deliverables;
    std::vector<std::string> granted_authorities;
    std::vector<std::string> success_signals;
    std::vector<std::string> fact_gaps;
    std::string executor_id;
    std::string executor_revision;
    std::optional<assurance::AcceptanceContract> acceptance_contract;
};

// Converts the durable PersistentTask requirement chain into the one canonical
// cognition input and commits the resulting plan into every downstream store.
class TaskPlanningService {
public:
    TaskPlanningService(TaskRegistry& tasks,
                        planning::MultiStageCognitionWorkflow& cognition,
                        harness::SQLiteProductionWorkflowInputRepository& inputs)
        : tasks_(tasks), cognition_(cognition), inputs_(inputs), orchestrator_(tasks) {}

    TaskPlanningResult plan(const PersistentTask& task,
                            const TaskPlanningPolicy& policy = {},
                            const planning::CognitionPipelineOptions& options = {});

private:
    TaskRegistry& tasks_;
    planning::MultiStageCognitionWorkflow& cognition_;
    harness::SQLiteProductionWorkflowInputRepository& inputs_;
    TaskOrchestrator orchestrator_;
};

}  // namespace agent_framework::conversation
