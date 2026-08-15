#pragma once

#include <memory>
#include <string>
#include <vector>

#include "agent/approval/store.hpp"
#include "agent/assurance/professional_workflow.hpp"
#include "agent/eval/judge_workflow.hpp"
#include "agent/execution/artifact_executor.hpp"
#include "agent/harness/run_binding_saga.hpp"
#include "agent/harness/cross_store_coordination.hpp"
#include "agent/harness/sqlite_production_input_repository.hpp"
#include "agent/llm_runtime/runtime.hpp"
#include "agent/memory_v2/store.hpp"
#include "agent/memory_v2/workflows/memory_workflow.hpp"
#include "agent/planning/cognition_pipeline.hpp"
#include "agent/remediation/remediation_workflow.hpp"
#include "agent/sandbox/provider.hpp"
#include "agent/telemetry/runtime.hpp"
#include "agent/ui/store_backed_operations.hpp"
#include "agent/tool_runtime/long_task_workflow.hpp"
#include "agent/tool_runtime/incremental_result_store.hpp"
#include "agent/tool_runtime/execution_control.hpp"
#include "agent/tool_runtime/orphan_recovery.hpp"
#include "agent/conversation/task_registry.hpp"

namespace agent_framework::harness {

struct ProductionCompositionDependencies {
    HarnessStore* harness_store{nullptr};
    run::RunStore* run_store{nullptr};
    SQLiteRunHarnessSaga* run_harness_saga{nullptr};
    CrossStoreCoordinator* cross_store_coordinator{nullptr};
    conversation::TaskRegistry* task_registry{nullptr};
    ProductionWorkflowInputRepository* input_repository{nullptr};
    planning::PlanStore* plan_store{nullptr};
    tool_runtime::LongTaskStore* long_task_store{nullptr};
    tool_runtime::InvocationStore* invocation_store{nullptr};
    tool_runtime::PlanNodeInputRepository* plan_node_input_repository{nullptr};
    tool_runtime::PlanNodeExecutorRegistry* plan_node_executor_registry{nullptr};
    tool_runtime::LongTaskWorkflow* long_task_workflow{nullptr};
    tool_runtime::LongTaskDispatcher* long_task_dispatcher{nullptr};
    tool_runtime::LongTaskTimerWorker* long_task_timer_worker{nullptr};
    tool_runtime::IncrementalResultStore* incremental_result_store{nullptr};
    tool_runtime::ExecutionControlStore* execution_control_store{nullptr};
    tool_runtime::InvocationOrphanSweeper* orphan_sweeper{nullptr};
    std::shared_ptr<llm_runtime::LLMRuntimeStore> llm_store;
    std::shared_ptr<llm_runtime::RoleRuntime> role_runtime;
    std::shared_ptr<telemetry::TelemetryRuntime> telemetry;
    approval::ApprovalStore* approval_store{nullptr};
    sandbox::SandboxProvider* sandbox_provider{nullptr};
    execution::WorkspaceArtifactExecutor* artifact_executor{nullptr};
    assurance::OracleRegistry* oracle_registry{nullptr};
    assurance::AssuranceStore* assurance_store{nullptr};
    memory_v2::MemoryStore* memory_store{nullptr};
    memory_v2::workflows::MemoryWorkflowCheckpointStore* memory_workflow_store{nullptr};
    remediation::RemediationStore* remediation_store{nullptr};
    eval::JudgeStore* judge_store{nullptr};
    StoreBackedOperationsAssembler* operations_assembler{nullptr};
    StoreBackedProductionWorkflowInputAssembler* input_assembler{nullptr};
    planning::MultiStageCognitionWorkflow* cognition_workflow{nullptr};
    memory_v2::workflows::MultiLayerMemoryWorkflow* memory_workflow{nullptr};
    assurance::ProfessionalAssuranceWorkflow* assurance_workflow{nullptr};
    remediation::LLMRemediationWorkflow* remediation_workflow{nullptr};
    eval::LLMJudgeWorkflow* judge_workflow{nullptr};
    std::string identity_verifier_manifest_digest;
    std::string policy_manifest_digest;
    std::string configuration_revision;
};

struct ProductionDependencyIssue { std::string code; std::string dependency; std::string message; };
struct ProductionDependencyReport {
    bool ready{false};
    std::string manifest_digest;
    std::vector<ProductionDependencyIssue> issues;
};

ProductionDependencyReport validate_production_dependencies(
    const ProductionCompositionDependencies& dependencies);

}  // namespace agent_framework::harness
