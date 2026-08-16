#pragma once

#include <memory>
#include <mutex>
#include <functional>
#include <string>
#include <vector>

#include "agent/conversation/harness_supported_runtime.hpp"
#include "agent/conversation/task_control_service.hpp"
#include "agent/conversation/task_planning_service.hpp"
#include "agent/harness/production_builder.hpp"
#include "agent/harness/task_closure.hpp"
#include "agent/recovery/task_state_coordinator.hpp"

namespace agent_framework::runtime {

// Assemble the only semantic completion decision accepted by the production
// Task coordinator from durable Assurance evidence. Missing, weak or
// unbound evidence remains non-verified.
harness::TaskClosureDecision evaluate_production_task_closure(
    harness::ProductionWorkflowInputRepository& repository,
    const contracts::ContractIdentity& identity,
    std::string_view acceptance_contract_digest,
    conversation::TaskExecutionProfile profile,
    const harness::HarnessCheckpoint& checkpoint);

// The deployment must retain every object referenced by dependencies through
// lifetime_anchors. This makes the raw dependency view accepted by the Phase 4
// builder safe for the complete lifetime of the interactive runtime.
struct ProductionRuntimeResources {
    harness::ProductionCompositionDependencies dependencies;
    harness::ProductionBoundaryAdapters boundaries;
    std::vector<std::shared_ptr<void>> lifetime_anchors;
    conversation::TaskPlanningPolicy planning_policy;
    recovery::SQLiteTaskCoordinationJournal* task_coordination_journal{nullptr};
    std::function<void(const recovery::CorrelatedStateEvent&,
                       const recovery::TaskCoordinationDecision&)>
        coordination_observer;
};

class ProductionLiveRuntime;

struct ProductionRuntimeBuildResult {
    std::shared_ptr<ProductionLiveRuntime> runtime;
    harness::ProductionBuildReport report;
    std::string error;
    explicit operator bool() const noexcept { return runtime != nullptr; }
};

class ProductionLiveRuntime final
    : public std::enable_shared_from_this<ProductionLiveRuntime> {
public:
    static ProductionRuntimeBuildResult build(ProductionRuntimeResources resources);

    conversation::HarnessSupportedTurnRuntime::Executor response_executor();
    conversation::HarnessSupportedTurnRuntime::Executor long_task_executor();
    std::shared_ptr<conversation::TaskControlService> task_control_service() const noexcept {
        return task_control_service_;
    }
    void set_coordination_observer(
        std::function<void(const recovery::CorrelatedStateEvent&,
                           const recovery::TaskCoordinationDecision&)> observer);
    const harness::ProductionBuildReport& report() const noexcept { return report_; }

private:
    ProductionLiveRuntime(ProductionRuntimeResources resources,
                          harness::Phase4HarnessRuntime harness_runtime,
                          harness::ProductionBuildReport report,
                          std::shared_ptr<tool_runtime::IncrementalResultViewAssembler> result_view,
                          std::shared_ptr<conversation::TaskControlService> task_control,
                          std::shared_ptr<conversation::TaskPlanningService> task_planning,
                          std::shared_ptr<recovery::DurableTaskStateCoordinator> task_coordinator);
    conversation::ModelTurnOutcome execute(
        const conversation::HarnessSupportedTurnRequest& request,
        bool task_scoped);

    ProductionRuntimeResources resources_;
    harness::Phase4HarnessRuntime harness_runtime_;
    harness::ProductionBuildReport report_;
    std::shared_ptr<tool_runtime::IncrementalResultViewAssembler> result_view_;
    std::shared_ptr<conversation::TaskControlService> task_control_service_;
    std::shared_ptr<conversation::TaskPlanningService> task_planning_service_;
    recovery::TaskStateCoordinator task_state_coordinator_;
    std::shared_ptr<recovery::DurableTaskStateCoordinator> durable_task_coordinator_;
    std::mutex execute_mutex_;
};

}  // namespace agent_framework::runtime
