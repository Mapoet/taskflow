#include "agent/harness/production_workflow_adapters.hpp"

#include <set>
#include <stdexcept>

#include "agent/contracts/contract.hpp"

namespace agent_framework::harness {
namespace {
WorkflowStageExecution failed(std::string code, std::string message) {
    WorkflowStageExecution out;
    out.result.outcome = StageOutcome::ManualReview;
    out.result.error_code = std::move(code);
    out.result.error_message = std::move(message);
    return out;
}

template <typename Artifacts>
std::vector<std::string> invocation_ids(const Artifacts& artifacts) {
    std::vector<std::string> ids;
    std::set<std::string> seen;
    for(const auto& artifact : artifacts)
        if(!artifact.invocation_id.empty() && seen.insert(artifact.invocation_id).second)
            ids.push_back(artifact.invocation_id);
    return ids;
}

std::string digest(const nlohmann::json& value) {
    return contracts::canonical_digest(value).value_or("");
}
}

std::optional<CognitionWorkflowInput> CallbackProductionWorkflowInputAssembler::cognition(
    const HarnessStageRequest& request, std::string* error) {
    if(!cognition_fn) { if(error) *error = "cognition input provider is not configured"; return std::nullopt; }
    return cognition_fn(request, error);
}
std::optional<MemoryWorkflowAdapterInput> CallbackProductionWorkflowInputAssembler::memory(
    const HarnessStageRequest& request, std::string* error) {
    if(!memory_fn) { if(error) *error = "memory input provider is not configured"; return std::nullopt; }
    return memory_fn(request, error);
}
std::optional<AssuranceWorkflowInput> CallbackProductionWorkflowInputAssembler::assurance(
    const HarnessStageRequest& request, bool reverification, std::string* error) {
    if(!assurance_fn) { if(error) *error = "assurance input provider is not configured"; return std::nullopt; }
    return assurance_fn(request, reverification, error);
}
std::optional<RemediationWorkflowInput> CallbackProductionWorkflowInputAssembler::remediation(
    const HarnessStageRequest& request, std::string* error) {
    if(!remediation_fn) { if(error) *error = "remediation input provider is not configured"; return std::nullopt; }
    return remediation_fn(request, error);
}
std::optional<JudgeWorkflowInput> CallbackProductionWorkflowInputAssembler::judge(
    const HarnessStageRequest& request, std::string* error) {
    if(!judge_fn) { if(error) *error = "judge input provider is not configured"; return std::nullopt; }
    return judge_fn(request, error);
}

StoreBackedProductionWorkflowInputAssembler::StoreBackedProductionWorkflowInputAssembler(
    ProductionWorkflowInputRepository& repository, planning::PlanStore& plans,
    memory_v2::MemoryScope subject)
    : repository_(repository), plans_(plans), subject_(std::move(subject)) {}

bool StoreBackedProductionWorkflowInputAssembler::identity_matches(
    const contracts::ContractIdentity& expected,
    const contracts::ContractIdentity& actual) const {
    return expected.tenant_id == actual.tenant_id && expected.task_id == actual.task_id &&
           expected.run_id == actual.run_id;
}

std::optional<CognitionWorkflowInput> StoreBackedProductionWorkflowInputAssembler::cognition(
    const HarnessStageRequest& request, std::string* error) {
    auto intake = repository_.intake(request.checkpoint.metadata.identity);
    if(!intake || !identity_matches(request.checkpoint.metadata.identity, intake->metadata.identity)) {
        if(error) *error = "store-backed intake missing or identity mismatch";
        return std::nullopt;
    }
    CognitionWorkflowInput input;
    input.intake = std::move(*intake); input.subject = subject_;
    input.options.pipeline_id = request.checkpoint.harness_id + ":cognition";
    input.options.approval_decision_id = request.checkpoint.pins.approval_decision_id;
    return input;
}

std::optional<MemoryWorkflowAdapterInput> StoreBackedProductionWorkflowInputAssembler::memory(
    const HarnessStageRequest& request, std::string* error) {
    auto value = repository_.memory_input(request.checkpoint.metadata.identity,
                                           request.checkpoint.pins.artifact_manifest_digest);
    if(!value || !identity_matches(request.checkpoint.metadata.identity, value->metadata.identity)) {
        if(error) *error = "store-backed memory input missing or identity mismatch";
        return std::nullopt;
    }
    MemoryWorkflowAdapterInput input; input.input = std::move(*value);
    input.options.approval_decision_id = request.checkpoint.pins.approval_decision_id;
    return input;
}

std::optional<AssuranceWorkflowInput> StoreBackedProductionWorkflowInputAssembler::assurance(
    const HarnessStageRequest& request, bool reverification, std::string* error) {
    auto contract = repository_.acceptance_contract(
        request.checkpoint.metadata.identity, request.checkpoint.pins.acceptance_contract_digest);
    auto context = repository_.task_context(
        request.checkpoint.metadata.identity, request.checkpoint.pins.plan_digest);
    auto artifact = repository_.artifact_manifest(
        request.checkpoint.metadata.identity, request.checkpoint.pins.artifact_manifest_digest);
    if(!contract || !context || !artifact ||
       !identity_matches(request.checkpoint.metadata.identity, contract->metadata.identity)) {
        if(error) *error = "store-backed assurance contract/context/artifact missing or identity mismatch";
        return std::nullopt;
    }
    AssuranceWorkflowInput input;
    input.contract = std::move(*contract); input.subject = subject_;
    input.task_context = std::move(*context); input.artifact_manifest = std::move(*artifact);
    input.options.workflow_id = request.checkpoint.harness_id +
        (reverification ? ":reverification" : ":assurance");
    return input;
}

std::optional<RemediationWorkflowInput> StoreBackedProductionWorkflowInputAssembler::remediation(
    const HarnessStageRequest& request, std::string* error) {
    auto plan = plans_.current(request.checkpoint.metadata.identity);
    auto contract = repository_.acceptance_contract(
        request.checkpoint.metadata.identity, request.checkpoint.pins.acceptance_contract_digest);
    auto report = repository_.acceptance_report(
        request.checkpoint.metadata.identity, request.checkpoint.pins.acceptance_report_digest);
    auto checkpoint = repository_.assurance_checkpoint(
        request.checkpoint.metadata.identity, request.checkpoint.pins.acceptance_report_digest);
    auto inventory = repository_.impact_inventory(
        request.checkpoint.metadata.identity, request.checkpoint.pins.artifact_manifest_digest);
    if(!plan || !contract || !report || !checkpoint || !inventory) {
        if(error) *error = "store-backed remediation inputs are incomplete";
        return std::nullopt;
    }
    RemediationWorkflowInput input;
    input.current_plan = std::move(*plan); input.contract = std::move(*contract);
    input.report = std::move(*report); input.assurance_checkpoint = std::move(*checkpoint);
    input.inventory = std::move(*inventory); input.subject = subject_;
    input.options.workflow_id = request.checkpoint.harness_id + ":remediation";
    input.options.approval_decision_id = request.checkpoint.pins.approval_decision_id;
    return input;
}

std::optional<JudgeWorkflowInput> StoreBackedProductionWorkflowInputAssembler::judge(
    const HarnessStageRequest& request, std::string* error) {
    auto input = repository_.evaluation_input(request.checkpoint.metadata.identity);
    if(!input || !input->datasets ||
       !identity_matches(request.checkpoint.metadata.identity, input->suite.metadata.identity)) {
        if(error) *error = "store-backed evaluation suite/runs/datasets missing or identity mismatch";
        return std::nullopt;
    }
    input->subject = subject_;
    input->options.workflow_id = request.checkpoint.harness_id + ":judge";
    input->options.approval_decision_id = request.checkpoint.pins.approval_decision_id;
    return input;
}

std::vector<llm_runtime::LLMInvocationManifest> InvocationManifestResolver::resolve(
    std::string_view tenant_id, const std::vector<std::string>& ids, std::string* error) const {
    std::vector<llm_runtime::LLMInvocationManifest> result;
    if(!store_) { if(error) *error = "LLM runtime store is not configured"; return result; }
    for(const auto& id : ids) {
        const auto stored = store_->load_invocation(tenant_id, id);
        if(!stored || stored->manifest.state != llm_runtime::InvocationState::Succeeded) {
            if(error) *error = "successful invocation manifest missing: " + id;
            return {};
        }
        const auto issues = llm_runtime::validate(stored->manifest);
        if(!issues.empty()) { if(error) *error = "invalid invocation manifest: " + id; return {}; }
        result.push_back(stored->manifest);
    }
    return result;
}

CognitionWorkflowAdapter::CognitionWorkflowAdapter(
    planning::MultiStageCognitionWorkflow& workflow, ProductionWorkflowInputAssembler& inputs,
    InvocationManifestResolver manifests, std::string revision, std::string config)
    : workflow_(workflow), inputs_(inputs), manifests_(std::move(manifests)),
      revision_(std::move(revision)), configuration_digest_(std::move(config)) {}

WorkflowStageExecution CognitionWorkflowAdapter::run(const HarnessStageRequest& request) {
    std::string error;
    auto input = inputs_.cognition(request, &error);
    if(!input) return failed("cognition_input_unavailable", error);
    if(digest(planning::encode(input->intake)) != request.checkpoint.pins.intake_digest)
        return failed("cognition_input_digest_mismatch", "intake does not match harness pin");
    input->options.cancelled = request.cancelled;
    auto value = workflow_.run(input->intake, input->subject, input->options);
    WorkflowStageExecution out;
    if(value.state == planning::CognitionPipelineState::AwaitingClarification ||
       value.state == planning::CognitionPipelineState::AwaitingApproval)
        out.result.outcome = StageOutcome::AwaitingApproval;
    else if(value.state == planning::CognitionPipelineState::Approved && value.plan)
        out.result.outcome = StageOutcome::Succeeded;
    else if(value.state == planning::CognitionPipelineState::Cancelled)
        out.result.outcome = StageOutcome::Cancelled;
    else out.result.outcome = StageOutcome::ManualReview;
    out.result.error_code = value.error_code; out.result.error_message = value.error_message;
    out.result.pins.plan_digest = value.checkpoint.plan_digest;
    out.result.pins.memory_snapshot_id = value.checkpoint.memory_snapshot_id;
    out.result.pins.memory_view_digest = value.checkpoint.memory_view_digest;
    out.result.output_digest = digest(planning::encode(value.checkpoint));
    out.invocations = manifests_.resolve(request.checkpoint.metadata.identity.tenant_id,
                                         invocation_ids(value.checkpoint.artifacts), &error);
    if(!error.empty()) return failed("cognition_invocation_evidence_invalid", error);
    return out;
}

MemoryWorkflowAdapter::MemoryWorkflowAdapter(
    memory_v2::workflows::MultiLayerMemoryWorkflow& workflow,
    ProductionWorkflowInputAssembler& inputs, InvocationManifestResolver manifests,
    std::string revision, std::string config)
    : workflow_(workflow), inputs_(inputs), manifests_(std::move(manifests)),
      revision_(std::move(revision)), configuration_digest_(std::move(config)) {}

WorkflowStageExecution MemoryWorkflowAdapter::run(const HarnessStageRequest& request) {
    std::string error;
    auto input = inputs_.memory(request, &error);
    if(!input) return failed("memory_input_unavailable", error);
    input->options.cancelled = request.cancelled;
    auto value = workflow_.run(input->input, input->options);
    WorkflowStageExecution out;
    using State = memory_v2::workflows::MemoryWorkflowState;
    if(value.state == State::Completed) out.result.outcome = StageOutcome::Succeeded;
    else if(value.state == State::AwaitingApproval || value.state == State::AwaitingClarification)
        out.result.outcome = StageOutcome::AwaitingApproval;
    else if(value.state == State::Cancelled) out.result.outcome = StageOutcome::Cancelled;
    else out.result.outcome = StageOutcome::ManualReview;
    out.result.error_code = value.error_code; out.result.error_message = value.error_message;
    out.result.pins.memory_snapshot_id = value.checkpoint.dynamic_snapshot_id;
    out.result.pins.memory_view_digest = value.checkpoint.dynamic_view_digest;
    out.result.output_digest = digest(memory_v2::workflows::encode(value.checkpoint));
    out.result.effect_receipt_digest = digest({{"workflow_id", value.checkpoint.workflow_id},
        {"revision", value.checkpoint.revision}, {"snapshot", value.checkpoint.dynamic_snapshot_id},
        {"view", value.checkpoint.dynamic_view_digest}});
    out.invocations = manifests_.resolve(request.checkpoint.metadata.identity.tenant_id,
                                         invocation_ids(value.checkpoint.artifacts), &error);
    if(!error.empty()) return failed("memory_invocation_evidence_invalid", error);
    return out;
}
std::optional<WorkflowStageExecution> MemoryWorkflowAdapter::reconcile(
    const HarnessStageRequest& request) { return run(request); }

AssuranceWorkflowAdapter::AssuranceWorkflowAdapter(
    assurance::ProfessionalAssuranceWorkflow& workflow, ProductionWorkflowInputAssembler& inputs,
    InvocationManifestResolver manifests, bool reverification, std::string revision,
    std::string config)
    : workflow_(workflow), inputs_(inputs), manifests_(std::move(manifests)),
      reverification_(reverification), revision_(std::move(revision)),
      configuration_digest_(std::move(config)) {}
std::string AssuranceWorkflowAdapter::id() const {
    return reverification_ ? "phase4.reverification.workflow" : "phase4.assurance.workflow";
}
WorkflowAdapterKind AssuranceWorkflowAdapter::kind() const noexcept {
    return reverification_ ? WorkflowAdapterKind::Reverification : WorkflowAdapterKind::Assurance;
}
HarnessStage AssuranceWorkflowAdapter::stage() const noexcept {
    return reverification_ ? HarnessStage::Reverification : HarnessStage::Assurance;
}
WorkflowStageExecution AssuranceWorkflowAdapter::run(const HarnessStageRequest& request) {
    std::string error;
    auto input = inputs_.assurance(request, reverification_, &error);
    if(!input) return failed("assurance_input_unavailable", error);
    if(digest(assurance::encode(input->contract)) != request.checkpoint.pins.acceptance_contract_digest)
        return failed("acceptance_contract_digest_mismatch", "contract does not match harness pin");
    input->options.cancelled = request.cancelled;
    auto value = workflow_.run(input->contract, input->subject, input->task_context,
                               input->artifact_manifest, input->options);
    WorkflowStageExecution out;
    if(value.state == assurance::AssuranceWorkflowState::Completed && value.report) {
        out.result.outcome = value.report->decision == assurance::AcceptanceDecision::Accepted
            ? StageOutcome::Succeeded : StageOutcome::NeedsRemediation;
        out.result.acceptance_decision = value.report->decision == assurance::AcceptanceDecision::Accepted
            ? "accepted" : "rejected";
        for(const auto& finding : value.report->findings) out.result.finding_ids.push_back(finding.finding_id);
    } else if(value.state == assurance::AssuranceWorkflowState::Cancelled)
        out.result.outcome = StageOutcome::Cancelled;
    else out.result.outcome = StageOutcome::ManualReview;
    out.result.error_code = value.error_code; out.result.error_message = value.error_message;
    out.result.pins.acceptance_report_digest = value.checkpoint.acceptance_report_digest;
    out.result.output_digest = digest(assurance::encode(value.checkpoint));
    out.invocations = manifests_.resolve(request.checkpoint.metadata.identity.tenant_id,
                                         invocation_ids(value.checkpoint.artifacts), &error);
    if(!error.empty()) return failed("assurance_invocation_evidence_invalid", error);
    return out;
}

RemediationWorkflowAdapter::RemediationWorkflowAdapter(
    remediation::LLMRemediationWorkflow& workflow, ProductionWorkflowInputAssembler& inputs,
    InvocationManifestResolver manifests, std::string revision, std::string config)
    : workflow_(workflow), inputs_(inputs), manifests_(std::move(manifests)),
      revision_(std::move(revision)), configuration_digest_(std::move(config)) {}
WorkflowStageExecution RemediationWorkflowAdapter::run(const HarnessStageRequest& request) {
    std::string error;
    auto input = inputs_.remediation(request, &error);
    if(!input) return failed("remediation_input_unavailable", error);
    if(digest(planning::encode(input->current_plan)) != request.checkpoint.pins.plan_digest)
        return failed("remediation_plan_digest_mismatch", "plan does not match harness pin");
    input->options.cancelled = request.cancelled;
    auto value = workflow_.run(input->current_plan, input->contract, input->report,
        input->assurance_checkpoint, input->inventory, input->subject, input->options);
    WorkflowStageExecution out;
    if(value.state == remediation::RemediationState::ReadyForExecution && value.proposed_plan)
        out.result.outcome = StageOutcome::Succeeded;
    else if(value.state == remediation::RemediationState::AwaitingApproval)
        out.result.outcome = StageOutcome::AwaitingApproval;
    else if(value.state == remediation::RemediationState::Cancelled)
        out.result.outcome = StageOutcome::Cancelled;
    else out.result.outcome = StageOutcome::ManualReview;
    out.result.error_code = value.error_code; out.result.error_message = value.error_message;
    out.result.pins.plan_digest = value.checkpoint.committed_plan_digest.empty()
        ? (value.proposed_plan ? digest(planning::encode(*value.proposed_plan)) : "")
        : value.checkpoint.committed_plan_digest;
    out.result.pins.approval_decision_id = value.checkpoint.approval_decision_id;
    out.result.output_digest = digest(remediation::encode(value.checkpoint));
    out.invocations = manifests_.resolve(request.checkpoint.metadata.identity.tenant_id,
                                         invocation_ids(value.checkpoint.artifacts), &error);
    if(!error.empty()) return failed("remediation_invocation_evidence_invalid", error);
    return out;
}

JudgeWorkflowAdapter::JudgeWorkflowAdapter(
    eval::LLMJudgeWorkflow& workflow, ProductionWorkflowInputAssembler& inputs,
    InvocationManifestResolver manifests, std::string revision, std::string config)
    : workflow_(workflow), inputs_(inputs), manifests_(std::move(manifests)),
      revision_(std::move(revision)), configuration_digest_(std::move(config)) {}
WorkflowStageExecution JudgeWorkflowAdapter::run(const HarnessStageRequest& request) {
    std::string error;
    auto input = inputs_.judge(request, &error);
    if(!input || !input->datasets) return failed("judge_input_unavailable",
        error.empty() ? "dataset registry is required" : error);
    input->options.cancelled = request.cancelled;
    auto value = workflow_.run(input->suite, *input->datasets, input->baseline,
        input->candidate, input->subject, input->options);
    WorkflowStageExecution out;
    if((value.state == eval::JudgeWorkflowState::Approved ||
        value.state == eval::JudgeWorkflowState::Rejected) && value.report)
        out.result.outcome = StageOutcome::Succeeded;
    else if(value.state == eval::JudgeWorkflowState::AwaitingApproval)
        out.result.outcome = StageOutcome::AwaitingApproval;
    else if(value.state == eval::JudgeWorkflowState::Cancelled)
        out.result.outcome = StageOutcome::Cancelled;
    else out.result.outcome = StageOutcome::ManualReview;
    out.result.error_code = value.error_code; out.result.error_message = value.error_message;
    out.result.pins.judge_report_digest = value.checkpoint.evaluation_report_digest;
    out.result.output_digest = digest(eval::encode(value.checkpoint));
    out.invocations = manifests_.resolve(request.checkpoint.metadata.identity.tenant_id,
                                         invocation_ids(value.checkpoint.artifacts), &error);
    if(!error.empty()) return failed("judge_invocation_evidence_invalid", error);
    return out;
}

}  // namespace agent_framework::harness
