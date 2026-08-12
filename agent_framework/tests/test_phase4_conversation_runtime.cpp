#include <agent/conversation/conversation_engine.hpp>
#include <agent/conversation/production_bridge.hpp>
#include <agent/conversation/context_projection.hpp>
#include <agent/conversation/graph_turn_adapter.hpp>
#include <agent/internal/platform_io.hpp>
#include "phase4_harness_test_support.hpp"
#include <cassert>
#include <filesystem>
using namespace agent_framework::conversation;
int main()
{
    auto path = std::filesystem::temp_directory_path() / ("conversation-" + std::to_string(agent_framework::internal::current_process_id()) + ".sqlite3");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    ContextProjectionManifest projection;
    projection.identity = {"tenant", "conversation"};
    projection.turn_id = "turn";
    projection.revision = 1;
    projection.profile_revision_digest = "profile";
    projection.prompt_revision_digest = "prompt";
    projection.segments = {
        {"contract", "cas://contract", "sha256:c", "task_closure_controller", "", 100, true},
        {"policy", "cas://policy", "sha256:p", "pdp", "", 50, true},
        {"citation", "cas://citation", "sha256:x", "source", "", 50, true}};
    auto projected = ContextProjector::build(projection);
    assert(projected && !projected->digest.empty());
    CompactBoundaryRecord boundary;
    boundary.identity = projection.identity;
    boundary.turn_id = "turn";
    boundary.revision = 1;
    boundary.summary_digest = "sha256:s";
    boundary.pre_tokens = 1000;
    boundary.post_tokens = 400;
    assert(ContextProjector::validate_boundary(*projected, boundary));
    projected->segments[0].truncation_reason = "budget";
    assert(!ContextProjector::validate_boundary(*projected, boundary));
    {
        SQLiteConversationStore store(path.string());
        int calls = 0;
        ConversationEngine engine(store, [&](const TurnRequest &, const TurnCheckpoint &)
                                  {++calls;ModelTurnOutcome o;o.reason=calls==1?ModelTurnStopReason::ToolRequested:ModelTurnStopReason::EndTurn;o.tool_receipt_refs=calls==1?std::vector<std::string>{"receipt"}:std::vector<std::string>{};o.candidate_answer=calls==1?"calling":"candidate";return o; });
        TurnRequest r{{"tenant", "conversation"}, "turn-1", "hello", TaskExecutionProfile::ReadOnlyAnalysis, 10};
        auto first = engine.start_turn(r);
        assert(first.error.empty() && first.checkpoint.phase == TurnPhase::AwaitingTool && !first.outcome.task_completion_verified);
        auto second = engine.continue_turn(r, TurnContinuationReason::ToolResultsAvailable);
        assert(second.error.empty() && second.checkpoint.phase == TurnPhase::Completed && !second.outcome.task_completion_verified);
        auto messages = store.messages(r.identity);
        assert(messages.size() == 3 && messages[1].parent_id == messages[0].message_id);
        auto events = store.events(r.identity);
        assert(events.size() == 3 && events.back().sequence == 3);
        assert(!engine.start_turn(r).error.empty());
        assert(engine.classify_input("/status") == InputDisposition::StatusQuery);
    }
    {
        SQLiteConversationStore reopened(path.string());
        auto m = reopened.messages({"tenant", "conversation"});
        assert(m.size() == 3);
        auto c = reopened.load_turn({"tenant", "conversation"}, "turn-1");
        assert(c && c->phase == TurnPhase::Completed);
    }
    std::filesystem::remove(path, ec);
    ModelTurnOutcome invalid;
    invalid.task_completion_verified = true;
    assert(!validate(invalid).empty() && !outcome_can_close_task(invalid));
    assert(!task_execution_profile("unknown"));
    assert(!TaskProfileRouter::route(TaskExecutionProfile::Conversation, true, true).allowed);
    agent_framework::assurance::AcceptanceContract a;
    a.metadata = phase4_harness_test::metadata("conversation-contract");
    a.criteria = {{"exists", agent_framework::assurance::VerificationLayer::Functional, "exists", "artifact", {}, "", true}};
    std::string error;
    auto c = closure_contract_from(a, TaskExecutionProfile::ArtifactDelivery, &error);
    assert(c && c->mandatory_criteria.size() == 1);
    agent_framework::ExecutionResult execution;
    execution.success = true;
    execution.status = agent_framework::ExecutionTerminalStatus::Completed;
    execution.outputs = {{"final_answer", "candidate"},
                         {"model_stop_reason", "model_turn_completed"},
                         {"task_completion_verified", true}};
    auto turn = GraphTurnAdapter::from_execution(execution);
    assert(turn.reason == ModelTurnStopReason::EndTurn);
    assert(!turn.task_completion_verified && turn.candidate_answer == "candidate");
    auto long_path = std::filesystem::temp_directory_path() /
        ("conversation-100-" + std::to_string(agent_framework::internal::current_process_id()) + ".sqlite3");
    std::filesystem::remove(long_path, ec);
    {
        SQLiteConversationStore long_store(long_path.string());
        ConversationEngine long_engine(long_store, [](const TurnRequest&, const TurnCheckpoint&) {
            ModelTurnOutcome out; out.reason=ModelTurnStopReason::EndTurn;
            out.candidate_answer="ok"; return out;
        });
        for(int i=0;i<100;++i) {
            TurnRequest request{{"tenant","long"},"turn-"+std::to_string(i),
                "input-"+std::to_string(i),TaskExecutionProfile::Conversation,1};
            assert(long_engine.start_turn(request).error.empty());
        }
        auto chain=long_store.messages({"tenant","long"});
        assert(chain.size()==200);
        for(std::size_t i=1;i<chain.size();++i)
            assert(chain[i].parent_id==chain[i-1].message_id);
    }
    { SQLiteConversationStore reopened(long_path.string());
      assert(reopened.messages({"tenant","long"}).size()==200); }
    std::filesystem::remove(long_path, ec);
}
