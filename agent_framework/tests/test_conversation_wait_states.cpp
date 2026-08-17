#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>

#include "agent/conversation/conversation_engine.hpp"
#include "agent/internal/platform_io.hpp"

int main() {
    using namespace agent_framework::conversation;
    const auto path = std::filesystem::temp_directory_path() /
        ("conversation-wait-states-" + std::to_string(
            agent_framework::internal::current_process_id()) + ".sqlite3");
    std::error_code error;
    std::filesystem::remove(path, error);
    SQLiteConversationStore store(path.string());
    const ConversationIdentity identity{"tenant", "conversation"};
    auto run = [&](std::string turn_id, ModelTurnStopReason reason) {
        ConversationEngine engine(store, [reason](const auto&, const auto&) {
            ModelTurnOutcome outcome;
            outcome.reason = reason;
            if(reason == ModelTurnStopReason::AwaitingInput) {
                outcome.clarification = "Which target?";
                outcome.candidate_answer = "Which target?";
            }
            return outcome;
        });
        TurnRequest request{identity, std::move(turn_id), "perform task",
                            TaskExecutionProfile::CodeChange, 4};
        return engine.start_turn(request);
    };
    const auto input = run("turn-input", ModelTurnStopReason::AwaitingInput);
    assert(input.error.empty() && input.checkpoint.phase == TurnPhase::AwaitingInput);
    assert(input.checkpoint.continuation == TurnContinuationReason::None);
    TurnRequest input_steer{identity,"turn-input","refined target",
                            TaskExecutionProfile::CodeChange,4};
    assert(ConversationEngine(store,{}).submit_user_input(
        input_steer,InputDisposition::AppendToCurrentTurn));
    const auto approval = run("turn-approval", ModelTurnStopReason::AwaitingApproval);
    assert(approval.error.empty() && approval.checkpoint.phase == TurnPhase::AwaitingInput);
    assert(approval.checkpoint.continuation == TurnContinuationReason::ResumeAfterApproval);
    TurnRequest approval_steer{identity,"turn-approval","approval context",
                               TaskExecutionProfile::CodeChange,4};
    assert(ConversationEngine(store,{}).submit_user_input(
        approval_steer,InputDisposition::AppendToCurrentTurn));
    const auto external = run("turn-external", ModelTurnStopReason::AwaitingExternal);
    assert(external.error.empty() && external.checkpoint.phase == TurnPhase::AwaitingTool);
    assert(external.checkpoint.continuation == TurnContinuationReason::ToolResultsAvailable);
    TurnRequest external_steer{identity,"turn-external","external result annotation",
                               TaskExecutionProfile::CodeChange,4};
    assert(ConversationEngine(store,{}).submit_user_input(
        external_steer,InputDisposition::AppendToCurrentTurn));
    const auto steered=store.inputs(identity,InputState::Consumed);
    assert(steered.size()==3);
    std::filesystem::remove(path, error);
}
