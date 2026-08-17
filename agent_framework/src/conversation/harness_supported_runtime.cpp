#include "agent/conversation/harness_supported_runtime.hpp"

#include <chrono>
#include <stdexcept>

namespace agent_framework::conversation {
namespace {
bool fallback_profile(TaskExecutionProfile profile) {
    return profile == TaskExecutionProfile::Conversation ||
           profile == TaskExecutionProfile::ReadOnlyAnalysis;
}
bool long_task_profile(TaskExecutionProfile profile) {
    return profile == TaskExecutionProfile::ArtifactDelivery ||
           profile == TaskExecutionProfile::CodeChange ||
           profile == TaskExecutionProfile::ExternalAction ||
           profile == TaskExecutionProfile::Professional;
}
}

const char* turn_execution_path_name(TurnExecutionPath path) noexcept {
    switch(path) {
        case TurnExecutionPath::Harness: return "harness";
        case TurnExecutionPath::LongTaskWorkflow: return "long_task_workflow";
        case TurnExecutionPath::LegacyReactFallback: return "legacy_react_fallback";
        case TurnExecutionPath::FailClosed: return "fail_closed";
    }
    return "fail_closed";
}

HarnessSupportedTurnRuntime::HarnessSupportedTurnRuntime(
    HarnessSupportedRuntimePolicy policy, Executor harness_executor,
    Executor legacy_fallback, RuntimeEventSink events, Executor long_task_executor)
    : policy_(policy), harness_executor_(std::move(harness_executor)),
      long_task_executor_(std::move(long_task_executor)),
      legacy_fallback_(std::move(legacy_fallback)), events_(std::move(events)) {}

TurnExecutionDecision HarnessSupportedTurnRuntime::route(
    const HarnessSupportedRuntimePolicy& policy, TaskExecutionProfile profile) {
    if(long_task_profile(profile)) {
        if(policy.long_task_ready)
            return {TurnExecutionPath::LongTaskWorkflow, "long_task_workflow_ready"};
        if(!policy.production && policy.harness_ready)
            return {TurnExecutionPath::Harness,
                    "nonproduction_long_task_harness_compatibility"};
        return {TurnExecutionPath::FailClosed,
                "long_task_workflow_unavailable_for_profile"};
    }
    if(policy.harness_ready)
        return {TurnExecutionPath::Harness, "harness_ready"};
    if(policy.production)
        return {TurnExecutionPath::FailClosed, "production_harness_unavailable"};
    if(policy.explicit_legacy_fallback && fallback_profile(profile))
        return {TurnExecutionPath::LegacyReactFallback,
                "explicit_nonproduction_compatibility_mode"};
    return {TurnExecutionPath::FailClosed,
            policy.explicit_legacy_fallback
                ? "legacy_fallback_forbidden_for_profile"
                : "harness_unavailable_and_fallback_not_explicit"};
}

TurnExecutionDecision HarnessSupportedTurnRuntime::route(
    const HarnessSupportedRuntimePolicy& policy,const TurnRequest& request) {
    // Planning is orthogonal to effect/profile. A complex read-only task uses
    // the durable workflow, while a direct conversational turn stays on Harness.
    if(request.planning_required||request.promotion_mode=="long_running_task"||
       request.promotion_mode=="continuous_task") {
        if(policy.long_task_ready)
            return {TurnExecutionPath::LongTaskWorkflow,"semantic_planning_workflow_ready"};
        if(!policy.production&&policy.harness_ready)
            return {TurnExecutionPath::Harness,"nonproduction_planning_harness_compatibility"};
        return {TurnExecutionPath::FailClosed,"planning_workflow_unavailable"};
    }
    return route(policy,request.profile);
}

ModelTurnOutcome HarnessSupportedTurnRuntime::execute(
    const TurnRequest& request, const TurnCheckpoint& checkpoint) const {
    const auto decision = route(policy_, request);
    emit(request, decision.path, decision.reason_code);
    const HarnessSupportedTurnRequest supported{request, checkpoint};
    ModelTurnOutcome outcome;
    switch(decision.path) {
        case TurnExecutionPath::Harness:
            if(!harness_executor_)
                throw std::runtime_error("harness_executor_missing_after_ready_route");
            outcome = harness_executor_(supported);
            break;
        case TurnExecutionPath::LongTaskWorkflow:
            if(!long_task_executor_)
                throw std::runtime_error("long_task_executor_missing_after_ready_route");
            outcome = long_task_executor_(supported);
            break;
        case TurnExecutionPath::LegacyReactFallback:
            if(!legacy_fallback_)
                throw std::runtime_error("explicit_legacy_fallback_executor_missing");
            outcome = legacy_fallback_(supported);
            break;
        case TurnExecutionPath::FailClosed:
            throw std::runtime_error(decision.reason_code);
    }
    // A model/executor turn may produce a completion candidate, but only the
    // TaskClosure authority may verify the task.  Keep the legacy field
    // fail-closed until it is removed from the wire contract.
    outcome.task_completion_verified = false;
    return outcome;
}

void HarnessSupportedTurnRuntime::emit(
    const TurnRequest& request, TurnExecutionPath path, std::string_view reason) const {
    if(!events_) return;
    RuntimeEventEnvelope event;
    event.event_id = request.turn_id + ":execution-path";
    event.tenant_id = request.identity.tenant_id;
    event.conversation_id = request.identity.conversation_id;
    event.turn_id = request.turn_id;
    event.durability = EventDurability::Durable;
    event.visibility = EventVisibility::Operations;
    event.event_type = "turn_execution_path_selected";
    event.timestamp = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    event.payload = {{"path", turn_execution_path_name(path)},
                     {"reason", reason},
                     {"production", policy_.production},
                     {"harness_ready", policy_.harness_ready},
                     {"long_task_ready", policy_.long_task_ready}};
    event.payload["work_shape"]=request.work_shape;
    event.payload["effect_class"]=request.effect_class;
    event.payload["assurance_tier"]=request.assurance_tier;
    event.payload["promotion_mode"]=request.promotion_mode;
    event.payload["planning_required"]=request.planning_required;
    event.payload["planning_depth"]=request.planning_depth;
    event.payload["routing_policy_revision"]=request.routing_policy_revision;
    events_(event);
}

}  // namespace agent_framework::conversation
