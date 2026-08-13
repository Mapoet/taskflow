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

    auto route = HarnessSupportedTurnRuntime::route({true, false, true}, request.profile);
    assert(route.path == TurnExecutionPath::FailClosed);
    route = HarnessSupportedTurnRuntime::route({false, false, true}, request.profile);
    assert(route.path == TurnExecutionPath::FailClosed);
    route = HarnessSupportedTurnRuntime::route(
        {false, false, true}, TaskExecutionProfile::Conversation);
    assert(route.path == TurnExecutionPath::LegacyReactFallback);
    route = HarnessSupportedTurnRuntime::route({true, true, true}, request.profile);
    assert(route.path == TurnExecutionPath::Harness);

    int harness_calls = 0, fallback_calls = 0;
    std::vector<RuntimeEventEnvelope> events;
    HarnessSupportedTurnRuntime runtime(
        {true, true, true},
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
        [&](const auto& event) { events.push_back(event); });
    const auto verified = runtime.execute(request, checkpoint);
    assert(verified.task_completion_verified && harness_calls == 1 && fallback_calls == 0);
    assert(events.size() == 1 && events[0].payload.at("path") == "harness");

    request.profile = TaskExecutionProfile::Conversation;
    HarnessSupportedTurnRuntime fallback(
        {false, false, true}, {},
        [&](const auto&) {
            ++fallback_calls;
            ModelTurnOutcome out;
            out.task_completion_verified = true;
            return out;
        });
    assert(!fallback.execute(request, checkpoint).task_completion_verified);

    HarnessSupportedTurnRuntime closed({true, false, true}, {}, fallback_calls
        ? HarnessSupportedTurnRuntime::Executor([&](const auto&) { return ModelTurnOutcome{}; })
        : HarnessSupportedTurnRuntime::Executor{});
    bool threw = false;
    try { (void)closed.execute(request, checkpoint); }
    catch(const std::runtime_error& e) {
        threw = std::string(e.what()) == "production_harness_unavailable";
    }
    assert(threw);
}
