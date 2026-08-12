#include "agent/harness/workflow_adapter.hpp"

#include <stdexcept>

#include "agent/contracts/contract.hpp"

namespace agent_framework::harness {
namespace {
const char* kind_name(WorkflowAdapterKind kind) {
    switch(kind) {
        case WorkflowAdapterKind::Intake: return "intake";
        case WorkflowAdapterKind::Cognition: return "cognition";
        case WorkflowAdapterKind::Approval: return "approval";
        case WorkflowAdapterKind::Execution: return "execution";
        case WorkflowAdapterKind::Memory: return "memory";
        case WorkflowAdapterKind::Assurance: return "assurance";
        case WorkflowAdapterKind::Remediation: return "remediation";
        case WorkflowAdapterKind::Reverification: return "reverification";
        case WorkflowAdapterKind::Judge: return "judge";
        case WorkflowAdapterKind::Operations: return "operations";
    }
    return "unknown";
}

bool kind_matches(WorkflowAdapterKind kind, HarnessStage stage) {
    switch(stage) {
        case HarnessStage::Intake: return kind == WorkflowAdapterKind::Intake;
        case HarnessStage::Cognition: return kind == WorkflowAdapterKind::Cognition;
        case HarnessStage::PlanApproval: return kind == WorkflowAdapterKind::Approval;
        case HarnessStage::Execution:
        case HarnessStage::Reexecution: return kind == WorkflowAdapterKind::Execution;
        case HarnessStage::MemoryUpdate: return kind == WorkflowAdapterKind::Memory;
        case HarnessStage::Assurance: return kind == WorkflowAdapterKind::Assurance;
        case HarnessStage::Remediation: return kind == WorkflowAdapterKind::Remediation;
        case HarnessStage::Reverification: return kind == WorkflowAdapterKind::Reverification;
        case HarnessStage::Judge: return kind == WorkflowAdapterKind::Judge;
        case HarnessStage::Operations: return kind == WorkflowAdapterKind::Operations;
        case HarnessStage::Complete: return false;
    }
    return false;
}
}

bool TelemetryLLMInvocationObserver::observe(
    const HarnessStageRequest& request,
    const llm_runtime::LLMInvocationManifest& manifest,
    std::string* error) {
    telemetry::SpanRecord span;
    span.context.metadata = request.checkpoint.metadata;
    span.context.trace_id = manifest.metadata.extensions.value(
        "trace_id", request.checkpoint.metadata.extensions.value(
            "trace_id", manifest.invocation_id));
    span.context.span_id = manifest.invocation_id;
    span.context.parent_span_id = request.checkpoint.metadata.extensions.value(
        "span_id", std::string{});
    span.context.node_id = harness_stage_name(request.stage);
    span.context.memory_snapshot_id = manifest.memory_snapshot_id;
    span.context.memory_view_digest = manifest.memory_view_digest;
    span.name = "gen_ai.role.invocation";
    span.started_at = manifest.started_at;
    span.finished_at = manifest.finished_at;
    span.status = llm_runtime::invocation_state_name(manifest.state);
    span.attributes = {
        {"gen_ai.operation.name", "role_invocation"},
        {"gen_ai.provider.name", manifest.provider},
        {"gen_ai.request.model", manifest.model},
        {"gen_ai.response.model", manifest.model},
        {"agent.role", manifest.role},
        {"agent.profile.id", manifest.profile_id},
        {"agent.profile.revision", manifest.profile_revision},
        {"agent.prompt.id", manifest.prompt_id},
        {"agent.prompt.revision", manifest.prompt_revision},
        {"agent.route.digest", manifest.route_decision_digest},
        {"agent.calibration.revision", manifest.calibration_revision},
        {"agent.fallback.count", std::to_string(manifest.attempts.size() > 1
            ? manifest.attempts.size() - 1 : 0)},
        {"agent.harness.stage", harness_stage_name(request.stage)},
        {"agent.invocation.manifest_digest",
         contracts::canonical_digest(llm_runtime::encode(manifest)).value_or("")}
    };
    if(!telemetry_.emit_span(std::move(span), error)) return false;
    auto emit = [&](std::string name, double value, std::string unit) {
        telemetry::MetricResult metric;
        metric.metadata = request.checkpoint.metadata;
        metric.metric_name = std::move(name);
        metric.value = value;
        metric.unit = std::move(unit);
        metric.outcome = llm_runtime::invocation_state_name(manifest.state);
        metric.sample_count = 1;
        return telemetry_.emit_metric(std::move(metric), error);
    };
    if(!emit("gen_ai.client.operation.duration", manifest.latency_ms, "ms")) return false;
    if(manifest.usage.input_tokens &&
       !emit("gen_ai.client.token.usage.input", *manifest.usage.input_tokens, "token")) return false;
    if(manifest.usage.output_tokens &&
       !emit("gen_ai.client.token.usage.output", *manifest.usage.output_tokens, "token")) return false;
    if(manifest.usage.cached_input_tokens &&
       !emit("gen_ai.client.token.usage.cached_input", *manifest.usage.cached_input_tokens, "token")) return false;
    if(manifest.usage.cost_usd &&
       !emit("gen_ai.client.cost", *manifest.usage.cost_usd, "USD")) return false;
    return true;
}

WorkflowHarnessStagePort::WorkflowHarnessStagePort(
    std::shared_ptr<TypedWorkflowAdapter> adapter,
    std::shared_ptr<LLMInvocationObserver> observer)
    : adapter_(std::move(adapter)), observer_(std::move(observer)) {
    if(!adapter_ || !observer_ || adapter_->id().empty() ||
       adapter_->implementation_revision().empty() ||
       adapter_->configuration_digest().empty() ||
       !kind_matches(adapter_->kind(), adapter_->stage()))
        throw std::invalid_argument("typed workflow adapter identity, kind, revision, configuration and observer are required");
    manifest_digest_ = contracts::canonical_digest({
        {"schema", "agent.typed_workflow_adapter/v1"}, {"adapter_id", adapter_->id()},
        {"kind", kind_name(adapter_->kind())}, {"stage", harness_stage_name(adapter_->stage())},
        {"side_effecting", adapter_->side_effecting()},
        {"implementation_revision", adapter_->implementation_revision()},
        {"configuration_digest", adapter_->configuration_digest()},
        {"llm_observability_required", true}}).value_or("");
}

std::string WorkflowHarnessStagePort::id() const { return adapter_->id(); }
bool WorkflowHarnessStagePort::may_have_side_effects() const noexcept {
    return adapter_->side_effecting();
}
bool WorkflowHarnessStagePort::production_ready() const noexcept {
    return !manifest_digest_.empty();
}
std::string WorkflowHarnessStagePort::capability_manifest_digest() const {
    return manifest_digest_;
}

HarnessStageResult WorkflowHarnessStagePort::finalize(
    const HarnessStageRequest& request, WorkflowStageExecution execution) {
    if(execution.invocations.empty() && (adapter_->kind() == WorkflowAdapterKind::Cognition ||
       adapter_->kind() == WorkflowAdapterKind::Memory ||
       adapter_->kind() == WorkflowAdapterKind::Assurance ||
       adapter_->kind() == WorkflowAdapterKind::Remediation ||
       adapter_->kind() == WorkflowAdapterKind::Reverification ||
       adapter_->kind() == WorkflowAdapterKind::Judge)) {
        execution.result.outcome = StageOutcome::ManualReview;
        execution.result.error_code = "llm_invocation_evidence_missing";
        execution.result.error_message = "LLM-driven workflow returned no invocation manifest";
        return execution.result;
    }
    nlohmann::json digests = nlohmann::json::array();
    for(const auto& invocation : execution.invocations) {
        std::string error;
        if(!observer_->observe(request, invocation, &error)) {
            execution.result.outcome = StageOutcome::ManualReview;
            execution.result.error_code = "llm_observability_export_failed";
            execution.result.error_message = error;
            return execution.result;
        }
        digests.push_back(contracts::canonical_digest(
            llm_runtime::encode(invocation)).value_or(""));
    }
    if(!digests.empty()) execution.result.invocation_manifest_digest =
        contracts::canonical_digest(digests).value_or("");
    return execution.result;
}

HarnessStageResult WorkflowHarnessStagePort::execute(const HarnessStageRequest& request) {
    if(request.stage != adapter_->stage() &&
       !(request.stage == HarnessStage::Reexecution &&
         adapter_->stage() == HarnessStage::Execution)) {
        HarnessStageResult result;
        result.outcome = StageOutcome::ManualReview;
        result.error_code = "workflow_stage_mismatch";
        result.error_message = "typed adapter bound to wrong stage";
        return result;
    }
    return finalize(request, adapter_->run(request));
}

std::optional<HarnessStageResult> WorkflowHarnessStagePort::reconcile(
    const HarnessStageRequest& request) {
    auto execution = adapter_->reconcile(request);
    if(!execution) return std::nullopt;
    return finalize(request, std::move(*execution));
}

}  // namespace agent_framework::harness
