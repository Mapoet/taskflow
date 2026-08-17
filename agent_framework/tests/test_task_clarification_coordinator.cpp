#include <cassert>
#include <filesystem>

#include "agent/conversation/task_clarification_coordinator.hpp"

using namespace agent_framework::conversation;

int main(){
    const auto path=(std::filesystem::temp_directory_path()/"agent-clarification-coordinator.sqlite").string();
    std::filesystem::remove(path);const ConversationIdentity identity{"tenant","conversation"};
    TurnRequest request{identity,"turn-1","modify the parser",TaskExecutionProfile::Conversation,5};
    request.task_id="task-1";request.run_id="run-1";
    TaskClassification weak;weak.profile=TaskExecutionProfile::CodeChange;
    weak.decision_id="decision-1";weak.requires_confirmation=true;weak.confidence=.5;
    {
        SQLiteConversationStore conversations(path);SQLiteTaskRegistry tasks(path);
        SQLiteTaskProfileClarificationStore clarifications(path);
        TaskClarificationCoordinator coordinator(conversations,clarifications,tasks);
        auto waiting=coordinator.begin(request,TaskInputIntent::InitialRequest,weak,100,1000);
        assert(waiting.error.empty()&&waiting.checkpoint.phase==TurnPhase::AwaitingInput);
        assert(!tasks.active(identity));
    }
    {
        SQLiteConversationStore conversations(path);SQLiteTaskRegistry tasks(path);
        SQLiteTaskProfileClarificationStore clarifications(path);
        TaskClarificationCoordinator coordinator(conversations,clarifications,tasks);
        int calls=0;auto resumed=coordinator.answer_pending(identity,"code_change",200,
            [&calls](const TurnRequest& restored,const TurnCheckpoint&){++calls;
                assert(restored.input=="modify the parser");assert(restored.task_id=="task-1");
                assert(restored.run_id=="run-1");assert(restored.profile==TaskExecutionProfile::CodeChange);
                ModelTurnOutcome out;out.reason=ModelTurnStopReason::EndTurn;
                out.candidate_answer="done";return out;});
        assert(resumed.handled&&resumed.resumed&&calls==1);
        auto active=tasks.active(identity);assert(active&&active->task_id=="task-1");
        assert(tasks.requirements(identity,"task-1").size()==1);
        assert(resumed.turn.checkpoint.phase==TurnPhase::Completed);
    }
    std::filesystem::remove(path);
}
