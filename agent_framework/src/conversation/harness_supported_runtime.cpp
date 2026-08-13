#include "agent/conversation/harness_supported_runtime.hpp"

#include <chrono>
#include <stdexcept>

namespace agent_framework::conversation {
namespace {
bool fallback_profile(TaskExecutionProfile profile) {
    return profile == TaskExecutionProfile::Conversation ||
           profile == TaskExecutionProfile::ReadOnlyAnalysis;
}
}

const char* turn_execution_path_name(TurnExecutionPath path) noexcept {
    switch(path) {
        case TurnExecutionPath::Harness: return "harness";
        case TurnExecutionPath::LegacyReactFallback: return "legacy_react_fallback";
        case TurnExecutionPath::FailClosed: return "fail_closed";
    }
    return "fail_closed";
}

HarnessSupportedTurnRuntime::HarnessSupportedTurnRuntime(
    HarnessSupportedRuntimePolicy policy, Executor harness_executor,
    Executor legacy_fallback, RuntimeEventSink events)
    : policy_(policy), harness_executor_(std::move(harness_executor)),
      legacy_fallback_(std::move(legacy_fallback)), events_(std::move(events)) {}

TurnExecutionDecision HarnessSupportedTurnRuntime::route(
    const HarnessSupportedRuntimePolicy& policy, TaskExecutionProfile profile) {
    if(policy.harness_ready)
        return {TurnExecutionPath::Harness, "harness_ready", true};
    if(policy.production)
        return {TurnExecutionPath::FailClosed, "production_harness_unavailable", false};
    if(policy.explicit_legacy_fallback && fallback_profile(profile))
        return {TurnExecutionPath::LegacyReactFallback,
                "explicit_nonproduction_compatibility_mode", false};
    return {TurnExecutionPath::FailClosed,
            policy.explicit_legacy_fallback
                ? "legacy_fallback_forbidden_for_profile"
                : "harness_unavailable_and_fallback_not_explicit", false};
}

ModelTurnOutcome HarnessSupportedTurnRuntime::execute(
    const TurnRequest& request, const TurnCheckpoint& checkpoint) const {
    const auto decision = route(policy_, request.profile);
    emit(request, decision.path, decision.reason_code);
    const HarnessSupportedTurnRequest supported{request, checkpoint};
    ModelTurnOutcome outcome;
    switch(decision.path) {
        case TurnExecutionPath::Harness:
            if(!harness_executor_)
                throw std::runtime_error("harness_executor_missing_after_ready_route");
            outcome = harness_executor_(supported);
            break;
        case TurnExecutionPath::LegacyReactFallback:
            if(!legacy_fallback_)
                throw std::runtime_error("explicit_legacy_fallback_executor_missing");
            outcome = legacy_fallback_(supported);
            outcome.task_completion_verified = false;
            break;
        case TurnExecutionPath::FailClosed:
            throw std::runtime_error(decision.reason_code);
    }
    if(!decision.completion_may_be_verified)
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
                     {"harness_ready", policy_.harness_ready}};
    events_(event);
}

}  // namespace agent_framework::conversation
