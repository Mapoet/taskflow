#include <cassert>
#include <filesystem>

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
    conversation::HarnessTurnAdapter adapter(store,
        [&](const auto&) {
            ++calls;
            conversation::ModelTurnOutcome out;
            out.reason = conversation::ModelTurnStopReason::EndTurn;
            out.candidate_answer = "candidate";
            return out;
        }, [&](const auto& value) { snapshot = value; });
    conversation::HarnessSupportedTurnRequest request;
    request.turn.identity = {"tenant", "conversation"};
    request.turn.turn_id = "turn-1";
    request.turn.input = "perform professional task";
    request.turn.profile = conversation::TaskExecutionProfile::Professional;
    request.checkpoint.identity = request.turn.identity;
    request.checkpoint.turn_id = request.turn.turn_id;
    const auto outcome = adapter.execute(request);
    assert(calls == 1 && !outcome.task_completion_verified);
    assert(snapshot.overall_status == OperationsStatus::Running);
    assert(snapshot.task_closure_state == "execution_completed_unverified");
    auto stored = store.load("tenant", "turn:turn-1");
    assert(stored && stored->checkpoint.state == harness::HarnessState::Completed);
    assert(harness::Phase4HarnessRuntime::completion_gate_issues(
        stored->checkpoint).empty());
    const auto again = adapter.execute(request);
    assert(!again.task_completion_verified && calls == 1); // durable resume, no re-execution
    fs::remove_all(root, ec);
}
