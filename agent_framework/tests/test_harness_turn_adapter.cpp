#include <cassert>
#include <filesystem>
#include <vector>

#include "agent/conversation/harness_turn_adapter.hpp"
#include "agent/harness/store.hpp"
#include "agent/internal/platform_io.hpp"

int main() {
    using namespace agent_framework;
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("harness-turn-" + std::to_string(internal::current_process_id()));
    std::error_code ec; fs::remove_all(root, ec); fs::create_directories(root);
    harness::SQLiteHarnessStore store((root / "harness.sqlite3").string());
    int calls = 0;
    Phase4OperationsSnapshot snapshot;
    std::vector<harness::HarnessEvent> trace;
    conversation::HarnessTurnAdapter adapter(store,
        [&](const auto&) {
            ++calls;
            conversation::ModelTurnOutcome out;
            out.reason = conversation::ModelTurnStopReason::EndTurn;
            out.candidate_answer = "candidate";
            return out;
        }, [&](const auto& value) { snapshot = value; },
        [&](const auto& event) { trace.push_back(event); });
    conversation::HarnessSupportedTurnRequest request;
    request.turn.identity = {"tenant", "conversation"};
    request.turn.turn_id = "turn-1";
    request.turn.input = "perform professional task";
    request.turn.profile = conversation::TaskExecutionProfile::Professional;
    request.turn.task_id = "task-1";
    request.turn.run_id = "run-1";
    request.checkpoint.identity = request.turn.identity;
    request.checkpoint.turn_id = request.turn.turn_id;
    const auto outcome = adapter.execute(request);
    assert(calls == 1 && !outcome.task_completion_verified);
    const auto checkpoint = store.load("tenant", "turn:turn-1");
    assert(checkpoint.has_value());
    assert(checkpoint->checkpoint.terminal_reason ==
           "pipeline_completed_unverified");
    assert(snapshot.overall_status == OperationsStatus::Running);
    assert(snapshot.task_closure_state == "execution_completed_unverified");
    auto stored = store.load("tenant", "turn:turn-1");
    assert(stored && stored->checkpoint.state == harness::HarnessState::Completed);
    assert(harness::Phase4HarnessRuntime::completion_gate_issues(
        stored->checkpoint).empty());
    assert(stored->checkpoint.metadata.identity.task_id == "task-1");
    assert(stored->checkpoint.metadata.identity.run_id == "run-1");
    assert(!trace.empty());
    bool typed_plan = false;
    for(const auto& event : trace) {
        if(event.event_type == "stage_result" &&
           event.payload.value("stage", "") == "cognition") {
            const auto& plan = event.payload.at("public_output");
            typed_plan = plan.value("schema", "") ==
                "agent.lightweight_conversation_plan/v1" &&
                plan.value("authority", "") == "non_authoritative";
        }
    }
    assert(typed_plan);
    const auto trace_size = trace.size();
    const auto again = adapter.execute(request);
    assert(!again.task_completion_verified && calls == 1); // durable resume, no re-execution
    assert(trace.size() == trace_size); // terminal replay emits no duplicate events

    auto second_request = request;
    second_request.turn.turn_id = "turn-2";
    second_request.turn.run_id = "run-2";
    const auto second = adapter.execute(second_request);
    assert(!second.task_completion_verified && calls == 2);
    assert(trace.size() > trace_size); // cursor is scoped per harness, not per adapter

    conversation::HarnessTurnAdapter empty_adapter(store,
        [](const auto&) {
            conversation::ModelTurnOutcome out;
            out.reason = conversation::ModelTurnStopReason::EndTurn;
            return out;
        });
    auto empty_request = request;
    empty_request.turn.turn_id = "turn-empty";
    empty_request.turn.run_id = "run-empty";
    const auto empty = empty_adapter.execute(empty_request);
    assert(empty.reason == conversation::ModelTurnStopReason::GuardStopped);
    assert(empty.candidate_answer.empty());
    assert(empty.candidate_answer.find("interactive_execution") == std::string::npos);
    const auto empty_checkpoint = store.load("tenant", "turn:turn-empty");
    assert(empty_checkpoint.has_value());
    assert(empty_checkpoint->checkpoint.state == harness::HarnessState::Failed);
    assert(empty_checkpoint->checkpoint.terminal_reason ==
           "interactive_execution_empty_delivery");
    const auto facing = conversation::user_facing_turn_failure(
        "interactive_execution_empty_delivery");
    assert(facing.find("interactive_execution") == std::string::npos);
    assert(!facing.empty());
    fs::remove_all(root, ec);
}
