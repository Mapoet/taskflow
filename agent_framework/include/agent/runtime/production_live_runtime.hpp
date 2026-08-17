#pragma once

#include <memory>
#include <mutex>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "agent/conversation/harness_supported_runtime.hpp"
#include "agent/conversation/task_control_service.hpp"
#include "agent/conversation/task_planning_service.hpp"
#include "agent/harness/production_builder.hpp"
#include "agent/harness/task_closure.hpp"
#include "agent/recovery/task_state_coordinator.hpp"
#include "agent/session/run_worker.hpp"

namespace agent_framework::runtime {
struct ProductionLiveRuntimeTestAccess;

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
    std::string deployment_profile;
    harness::ProductionCompositionDependencies dependencies;
    harness::ProductionBoundaryAdapters boundaries;
    std::vector<std::shared_ptr<void>> lifetime_anchors;
    // Every raw dependency exposed to the production builder must have a
    // deployment-owned, named lifetime anchor. Names are part of the signed
    // readiness manifest and make omissions diagnosable instead of relying on
    // one unrelated catch-all shared_ptr.
    std::map<std::string, std::shared_ptr<void>> owned_resources;
    conversation::TaskPlanningPolicy planning_policy;
    recovery::SQLiteTaskCoordinationJournal* task_coordination_journal{nullptr};
    std::function<void(const recovery::CorrelatedStateEvent&,
                       const recovery::TaskCoordinationDecision&)>
        coordination_observer;
};

struct ProductionOwnershipReport {
    bool ready{false};
    std::vector<std::string> missing;
    std::vector<std::string> mismatched;
    std::string manifest_digest;
};

ProductionOwnershipReport validate_production_runtime_ownership(
    const ProductionRuntimeResources& resources);

class ProductionLiveRuntime;

struct ProductionRuntimeBuildResult {
    std::shared_ptr<ProductionLiveRuntime> runtime;
    harness::ProductionBuildReport report;
    ProductionOwnershipReport ownership;
    std::string error;
    explicit operator bool() const noexcept { return runtime != nullptr; }
};

class ProductionLiveRuntime final
    : public std::enable_shared_from_this<ProductionLiveRuntime> {
public:
    static ProductionRuntimeBuildResult build(ProductionRuntimeResources resources);

    conversation::HarnessSupportedTurnRuntime::Executor response_executor();
    conversation::HarnessSupportedTurnRuntime::Executor long_task_executor();
    // The only supported bridge from a leased product Run into the production
    // harness.  Keeping this factory on ProductionLiveRuntime prevents a test
    // callback from being labelled as a production SessionRunWorker adapter.
    session::SessionRunWorker::Executor session_run_executor();
    std::shared_ptr<conversation::TaskControlService> task_control_service() const noexcept {
        return task_control_service_;
    }
    void set_coordination_observer(
        std::function<void(const recovery::CorrelatedStateEvent&,
                           const recovery::TaskCoordinationDecision&)> observer);
    // Canonical ingress for terminal observations emitted by Conversation,
    // Harness, Run, Invocation, Effect and Closure producers.  Producers do
    // not own Task lifecycle decisions; this runtime correlates and journals
    // them through the single production coordinator.
    bool publish_terminal_boundary(
        recovery::CoordinationBoundary boundary,
        recovery::CorrelatedStateEvent event,
        std::string* error = nullptr);
    const harness::ProductionBuildReport& report() const noexcept { return report_; }
    const ProductionOwnershipReport& ownership_report() const noexcept {
        return ownership_report_;
    }
    nlohmann::json readiness_manifest() const;

private:
    friend struct ProductionLiveRuntimeTestAccess;
    static ProductionRuntimeBuildResult assemble_validated(
        ProductionRuntimeResources resources,
        harness::Phase4HarnessRuntime harness_runtime,
        harness::ProductionBuildReport report,
        ProductionOwnershipReport ownership);
    ProductionLiveRuntime(ProductionRuntimeResources resources,
                          harness::Phase4HarnessRuntime harness_runtime,
                          harness::ProductionBuildReport report,
                          ProductionOwnershipReport ownership,
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
    ProductionOwnershipReport ownership_report_;
    std::shared_ptr<tool_runtime::IncrementalResultViewAssembler> result_view_;
    std::shared_ptr<conversation::TaskControlService> task_control_service_;
    std::shared_ptr<conversation::TaskPlanningService> task_planning_service_;
    recovery::TaskStateCoordinator task_state_coordinator_;
    std::shared_ptr<recovery::DurableTaskStateCoordinator> durable_task_coordinator_;
    std::mutex execute_mutex_;
};

}  // namespace agent_framework::runtime
