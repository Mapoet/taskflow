#include "agent/harness/production_boundary_adapters.hpp"

#include "agent/contracts/contract.hpp"

namespace agent_framework::harness {
namespace {
WorkflowStageExecution failure(std::string code, std::string message,
                               StageOutcome outcome = StageOutcome::ManualReview) {
    WorkflowStageExecution out;
    out.result.outcome = outcome;
    out.result.error_code = std::move(code);
    out.result.error_message = std::move(message);
    return out;
}
bool same_identity(const contracts::ContractIdentity& a,
                   const contracts::ContractIdentity& b) {
    return a.tenant_id == b.tenant_id && a.task_id == b.task_id &&
           (a.run_id.empty() || a.run_id == b.run_id);
}
}

StoreBackedIntakeAdapter::StoreBackedIntakeAdapter(
    ProductionWorkflowInputRepository& repository, std::string revision,
    std::string configuration_digest)
    : repository_(repository), revision_(std::move(revision)),
      configuration_digest_(std::move(configuration_digest)) {}
WorkflowStageExecution StoreBackedIntakeAdapter::run(const HarnessStageRequest& request) {
    const auto intake = repository_.intake(request.checkpoint.metadata.identity);
    if(!intake || !same_identity(intake->metadata.identity,
                                 request.checkpoint.metadata.identity))
        return failure("intake_not_found_or_identity_mismatch",
                       "durable intake is unavailable for this identity");
    const auto digest = planning::encode(*intake).at("canonical_digest").get<std::string>();
    if(digest != request.checkpoint.pins.intake_digest)
        return failure("intake_revision_mismatch", "pinned intake digest changed");
    WorkflowStageExecution out;
    out.result.outcome = StageOutcome::Succeeded;
    out.result.output_digest = digest;
    out.result.invocation_manifest_digest = request.request_digest;
    return out;
}

StoreBackedApprovalAdapter::StoreBackedApprovalAdapter(
    approval::ApprovalStore& store, std::function<std::string()> now,
    std::string revision, std::string configuration_digest)
    : store_(store), now_(std::move(now)), revision_(std::move(revision)),
      configuration_digest_(std::move(configuration_digest)) {}
WorkflowStageExecution StoreBackedApprovalAdapter::run(const HarnessStageRequest& request) {
    const auto approval_id = request.checkpoint.remediation_cycle == 0
        ? request.checkpoint.harness_id + ":plan-approval"
        : request.checkpoint.harness_id + ":remediation-approval:" +
          std::to_string(request.checkpoint.remediation_cycle);
    const auto approval_request = store_.request(approval_id);
    const auto decision = store_.latest_decision(approval_id);
    if(!approval_request || !decision) {
        auto out = failure("approval_pending", "accountable plan approval is pending",
                           StageOutcome::AwaitingApproval);
        out.result.invocation_manifest_digest = request.request_digest;
        return out;
    }
    const auto now = now_ ? now_() : std::string{};
    const auto request_digest = approval::encode(*approval_request)
        .at("canonical_digest").get<std::string>();
    const bool valid = same_identity(approval_request->metadata.identity,
                                     request.checkpoint.metadata.identity) &&
        decision->decision == approval::Decision::Approved &&
        decision->request_digest == request_digest &&
        decision->plan_digest == request.checkpoint.pins.plan_digest &&
        approval_request->plan_digest == request.checkpoint.pins.plan_digest &&
        decision->policy_revision == approval_request->policy_revision &&
        (decision->expires_at.empty() || now < decision->expires_at) &&
        (approval_request->expires_at.empty() || now < approval_request->expires_at);
    if(!valid) return failure("approval_binding_invalid",
        "approval identity, digest, policy, state or expiry mismatch",
        StageOutcome::Rejected);
    WorkflowStageExecution out;
    out.result.outcome = StageOutcome::Succeeded;
    out.result.invocation_manifest_digest = request.request_digest;
    out.result.output_digest = contracts::canonical_digest(
        approval::encode(*decision)).value_or("");
    out.result.pins.approval_decision_id = approval_id;
    return out;
}

ArtifactExecutionWorkflowAdapter::ArtifactExecutionWorkflowAdapter(
    execution::WorkspaceArtifactExecutor& executor, execution::ArtifactAction action,
    execution::ArtifactJournal& journal, std::string revision,
    std::string configuration_digest, std::string parent_idempotency_key)
    : port_("phase4.execution.artifact.port", executor, std::move(action), &journal,
            std::move(parent_idempotency_key)), revision_(std::move(revision)),
      configuration_digest_(std::move(configuration_digest)) {}
WorkflowStageExecution ArtifactExecutionWorkflowAdapter::run(
    const HarnessStageRequest& request) { return {port_.execute(request), {}}; }
std::optional<WorkflowStageExecution> ArtifactExecutionWorkflowAdapter::reconcile(
    const HarnessStageRequest& request) {
    auto result = port_.reconcile(request);
    return result ? std::optional<WorkflowStageExecution>{{std::move(*result), {}}}
                  : std::nullopt;
}

StoreBackedOperationsAdapter::StoreBackedOperationsAdapter(
    StoreBackedOperationsAssembler& assembler, memory_v2::MemoryScope subject,
    std::function<std::string()> now, std::string revision,
    std::string configuration_digest)
    : assembler_(assembler), subject_(std::move(subject)), now_(std::move(now)),
      revision_(std::move(revision)), configuration_digest_(std::move(configuration_digest)) {}
WorkflowStageExecution StoreBackedOperationsAdapter::invoke(
    const HarnessStageRequest& request) {
    OperationsAssemblyRequest input;
    input.tenant_id = request.checkpoint.metadata.identity.tenant_id;
    input.harness_id = request.checkpoint.harness_id;
    input.principal_id = request.checkpoint.metadata.identity.principal_id;
    input.now = now_ ? now_() : std::string{};
    input.memory_subject = subject_;
    std::string error;
    const auto snapshot = assembler_.assemble(input, &error);
    if(!snapshot) return failure("operations_assembly_failed", error);
    WorkflowStageExecution out;
    out.result.outcome = StageOutcome::Succeeded;
    out.result.invocation_manifest_digest = request.request_digest;
    out.result.output_digest = snapshot->snapshot_id;
    out.result.effect_receipt_digest = snapshot->snapshot_id;
    out.result.pins.operations_snapshot_digest = snapshot->snapshot_id;
    return out;
}
WorkflowStageExecution StoreBackedOperationsAdapter::run(
    const HarnessStageRequest& request) { return invoke(request); }
std::optional<WorkflowStageExecution> StoreBackedOperationsAdapter::reconcile(
    const HarnessStageRequest& request) { return invoke(request); }

}  // namespace agent_framework::harness
