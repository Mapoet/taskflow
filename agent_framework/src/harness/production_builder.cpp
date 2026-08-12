#include "agent/harness/production_builder.hpp"

#include <array>

#include "agent/contracts/contract.hpp"

namespace agent_framework::harness {
namespace {
bool validate_boundary(const std::shared_ptr<TypedWorkflowAdapter>& adapter,
                       WorkflowAdapterKind kind, HarnessStage stage,
                       ProductionBuildReport& report) {
    if(adapter && adapter->kind() == kind && adapter->stage() == stage &&
       !adapter->implementation_revision().empty() &&
       !adapter->configuration_digest().empty()) return true;
    report.composition_issues.push_back({"production_boundary_adapter_invalid", stage,
        "deployment must supply the correctly typed, revisioned production boundary adapter"});
    return false;
}

std::shared_ptr<WorkflowHarnessStagePort> port(
    std::shared_ptr<TypedWorkflowAdapter> adapter,
    const std::shared_ptr<LLMInvocationObserver>& observer) {
    return std::make_shared<WorkflowHarnessStagePort>(std::move(adapter), observer);
}
}  // namespace

std::optional<Phase4HarnessRuntime> DefaultProductionCompositionBuilder::build(
    const ProductionBoundaryAdapters& boundary, ProductionBuildReport* output,
    std::string* error) const {
    ProductionBuildReport local;
    const auto dependencies = validate_production_dependencies(dependencies_);
    local.dependency_issues = dependencies.issues;
    local.dependency_manifest_digest = dependencies.manifest_digest;

    validate_boundary(boundary.intake, WorkflowAdapterKind::Intake,
                      HarnessStage::Intake, local);
    validate_boundary(boundary.approval, WorkflowAdapterKind::Approval,
                      HarnessStage::PlanApproval, local);
    validate_boundary(boundary.execution, WorkflowAdapterKind::Execution,
                      HarnessStage::Execution, local);
    validate_boundary(boundary.operations, WorkflowAdapterKind::Operations,
                      HarnessStage::Operations, local);
    if(!dependencies.ready || !local.composition_issues.empty()) {
        if(error) *error = !dependencies.ready
            ? dependencies.issues.front().code + ":" + dependencies.issues.front().message
            : local.composition_issues.front().code + ":" + local.composition_issues.front().message;
        if(output) *output = std::move(local);
        return std::nullopt;
    }

    auto observer = std::make_shared<TelemetryLLMInvocationObserver>(*dependencies_.telemetry);
    InvocationManifestResolver resolver(dependencies_.llm_store);
    const auto& revision = dependencies_.configuration_revision;
    const auto config = dependencies.manifest_digest;
    auto cognition = std::make_shared<CognitionWorkflowAdapter>(
        *dependencies_.cognition_workflow, *dependencies_.input_assembler,
        resolver, revision, config);
    auto memory = std::make_shared<MemoryWorkflowAdapter>(
        *dependencies_.memory_workflow, *dependencies_.input_assembler,
        resolver, revision, config);
    auto assurance = std::make_shared<AssuranceWorkflowAdapter>(
        *dependencies_.assurance_workflow, *dependencies_.input_assembler,
        resolver, false, revision, config);
    auto remediation = std::make_shared<RemediationWorkflowAdapter>(
        *dependencies_.remediation_workflow, *dependencies_.input_assembler,
        resolver, revision, config);
    auto reverification = std::make_shared<AssuranceWorkflowAdapter>(
        *dependencies_.assurance_workflow, *dependencies_.input_assembler,
        resolver, true, revision, config);
    auto judge = std::make_shared<JudgeWorkflowAdapter>(
        *dependencies_.judge_workflow, *dependencies_.input_assembler,
        resolver, revision, config);

    auto saga_observer=std::shared_ptr<HarnessCheckpointObserver>(
        dependencies_.run_harness_saga, [](HarnessCheckpointObserver*){});
    Phase4ProductionComposition composition(*dependencies_.harness_store,
                                            *dependencies_.run_store, saga_observer);
    composition.bind(HarnessStage::Intake, port(boundary.intake, observer));
    composition.bind(HarnessStage::Cognition, port(cognition, observer));
    composition.bind(HarnessStage::PlanApproval, port(boundary.approval, observer));
    composition.bind(HarnessStage::Execution, port(boundary.execution, observer));
    composition.bind(HarnessStage::MemoryUpdate, port(memory, observer));
    composition.bind(HarnessStage::Assurance, port(assurance, observer));
    composition.bind(HarnessStage::Remediation, port(remediation, observer));
    composition.bind(HarnessStage::Reexecution, port(boundary.execution, observer));
    composition.bind(HarnessStage::Reverification, port(reverification, observer));
    composition.bind(HarnessStage::Judge, port(judge, observer));
    composition.bind(HarnessStage::Operations, port(boundary.operations, observer));

    const auto composition_report = composition.validate();
    local.composition_issues = composition_report.issues;
    local.composition_manifest_digest = composition_report.manifest_digest;
    local.ready = composition_report.ready;
    if(local.ready) local.deployment_manifest_digest = contracts::canonical_digest({
        {"schema", "agent.production_deployment/v1"},
        {"dependencies", local.dependency_manifest_digest},
        {"composition", local.composition_manifest_digest},
        {"configuration_revision", revision}}).value_or("");
    auto runtime = composition.build(error);
    if(!runtime) local.ready = false;
    if(output) *output = local;
    return runtime;
}

}  // namespace agent_framework::harness
