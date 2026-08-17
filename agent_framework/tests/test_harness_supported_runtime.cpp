#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <stdexcept>
#include <vector>

#include "agent/conversation/harness_supported_runtime.hpp"

int main() {
    using namespace agent_framework::conversation;
    auto request = TurnRequest{{"tenant", "conversation"}, "turn", "work",
                               TaskExecutionProfile::Professional, 10};
    TurnCheckpoint checkpoint;
    checkpoint.identity = request.identity;
    checkpoint.turn_id = request.turn_id;

    auto route = HarnessSupportedTurnRuntime::route({true, false, false, true}, request.profile);
    assert(route.path == TurnExecutionPath::FailClosed);
    route = HarnessSupportedTurnRuntime::route({false, false, false, true}, request.profile);
    assert(route.path == TurnExecutionPath::FailClosed);
    route = HarnessSupportedTurnRuntime::route(
        {false, false, false, true}, TaskExecutionProfile::Conversation);
    assert(route.path == TurnExecutionPath::LegacyReactFallback);
    route = HarnessSupportedTurnRuntime::route({true, true, true, true}, request.profile);
    assert(route.path == TurnExecutionPath::LongTaskWorkflow);
    request.profile=TaskExecutionProfile::ReadOnlyAnalysis;
    request.planning_required=true;request.promotion_mode="long_running_task";
    route=HarnessSupportedTurnRuntime::route({true,true,true,false},request);
    assert(route.path==TurnExecutionPath::LongTaskWorkflow);
    request.planning_required=false;request.promotion_mode="direct_turn";
    route=HarnessSupportedTurnRuntime::route({true,true,true,false},request);
    assert(route.path==TurnExecutionPath::Harness);
    request.profile=TaskExecutionProfile::Professional;
    for(const auto profile : {TaskExecutionProfile::Conversation,
                              TaskExecutionProfile::ReadOnlyAnalysis}) {
        assert(HarnessSupportedTurnRuntime::route(
            {true,true,true,true},profile).path==TurnExecutionPath::Harness);
        assert(HarnessSupportedTurnRuntime::route(
            {false,false,false,true},profile).path==TurnExecutionPath::LegacyReactFallback);
    }
    for(const auto profile : {TaskExecutionProfile::ArtifactDelivery,
                              TaskExecutionProfile::CodeChange,
                              TaskExecutionProfile::ExternalAction,
                              TaskExecutionProfile::Professional}) {
        assert(HarnessSupportedTurnRuntime::route(
            {true,true,true,true},profile).path==TurnExecutionPath::LongTaskWorkflow);
        assert(HarnessSupportedTurnRuntime::route(
            {true,true,false,true},profile).path==TurnExecutionPath::FailClosed);
        assert(HarnessSupportedTurnRuntime::route(
            {false,true,false,true},profile).path==TurnExecutionPath::Harness);
    }

    int harness_calls = 0, fallback_calls = 0;
    std::vector<RuntimeEventEnvelope> events;
    HarnessSupportedTurnRuntime runtime(
        {true, true, true, true},
        [&](const auto&) {
            ++harness_calls;
            ModelTurnOutcome out;
            out.candidate_answer = "verified";
            out.task_completion_verified = true;
            return out;
        },
        [&](const auto&) {
            ++fallback_calls;
            ModelTurnOutcome out;
            out.task_completion_verified = true;
            return out;
        },
        [&](const auto& event) { events.push_back(event); },
        [&](const auto&) {
            ++harness_calls;
            ModelTurnOutcome out;
            out.candidate_answer = "long-task-accepted";
            return out;
        });
    const auto verified = runtime.execute(request, checkpoint);
    assert(!verified.task_completion_verified && harness_calls == 1 && fallback_calls == 0);
    assert(events.size() == 1 && events[0].payload.at("path") == "long_task_workflow");

    request.profile = TaskExecutionProfile::Conversation;
    HarnessSupportedTurnRuntime fallback(
        {false, false, false, true}, {},
        [&](const auto&) {
            ++fallback_calls;
            ModelTurnOutcome out;
            out.task_completion_verified = true;
            return out;
        });
    assert(!fallback.execute(request, checkpoint).task_completion_verified);

    HarnessSupportedTurnRuntime closed({true, false, false, true}, {}, fallback_calls
        ? HarnessSupportedTurnRuntime::Executor([&](const auto&) { return ModelTurnOutcome{}; })
        : HarnessSupportedTurnRuntime::Executor{});
    bool threw = false;
    try { (void)closed.execute(request, checkpoint); }
    catch(const std::runtime_error& e) {
        threw = std::string(e.what()) == "production_harness_unavailable";
    }
    assert(threw);

    // Typed durable waits survive the routing boundary unchanged.
    for(const auto reason : {ModelTurnStopReason::AwaitingInput,
                             ModelTurnStopReason::AwaitingApproval,
                             ModelTurnStopReason::AwaitingExternal}) {
        HarnessSupportedTurnRuntime waiting({true,true,true,false},
            [reason](const auto&){ModelTurnOutcome out;out.reason=reason;return out;},
            {},{},[reason](const auto&){ModelTurnOutcome out;out.reason=reason;return out;});
        request.profile=TaskExecutionProfile::Professional;
        assert(waiting.execute(request,checkpoint).reason==reason);
    }
}
