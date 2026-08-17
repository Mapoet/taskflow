#include <cassert>
#include <chrono>
#include <filesystem>
#include <atomic>
#include <barrier>
#include <thread>

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
        auto runs=tasks.runs(identity,"task-1");assert(runs.size()==1);
        assert(runs[0].classification_decision_id=="decision-1"&&
               runs[0].clarification_id=="decision-1");
        assert(resumed.turn.checkpoint.phase==TurnPhase::Completed);
    }
    const auto race_path=(std::filesystem::temp_directory_path()/"agent-clarification-resume-race.sqlite").string();
    std::filesystem::remove(race_path);
    TurnRequest raced_request{identity,"turn-race","change code",TaskExecutionProfile::Conversation,5};
    raced_request.task_id="task-race";raced_request.run_id="run-race";
    weak.decision_id="decision-race";
    {
        SQLiteConversationStore conversations(race_path);SQLiteTaskRegistry tasks(race_path);
        SQLiteTaskProfileClarificationStore clarifications(race_path);
        TaskClarificationCoordinator coordinator(conversations,clarifications,tasks);
        assert(coordinator.begin(raced_request,TaskInputIntent::InitialRequest,
                                 weak,100,1000).error.empty());
    }
    {
        SQLiteConversationStore conversations1(race_path),conversations2(race_path);
        SQLiteTaskRegistry tasks1(race_path),tasks2(race_path);
        SQLiteTaskProfileClarificationStore clarifications1(race_path),clarifications2(race_path);
        TaskClarificationCoordinator first(conversations1,clarifications1,tasks1);
        TaskClarificationCoordinator second(conversations2,clarifications2,tasks2);
        std::barrier ready(3);std::atomic<int> executions{0};
        ClarificationResumeResult a,b;
        auto executor=[&](const TurnRequest&,const TurnCheckpoint&){++executions;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            ModelTurnOutcome out;out.reason=ModelTurnStopReason::EndTurn;return out;};
        std::thread one([&]{ready.arrive_and_wait();a=first.answer_pending(
            identity,"code_change",200,executor);});
        std::thread two([&]{ready.arrive_and_wait();b=second.answer_pending(
            identity,"code_change",200,executor);});
        ready.arrive_and_wait();one.join();two.join();
        assert(a.handled&&b.handled&&a.error.empty()&&b.error.empty());
        assert(executions==1);
        auto active=tasks1.active(identity);assert(active);
        assert(tasks1.requirements(identity,active->task_id).size()==1);
    }
    std::filesystem::remove(race_path);
    std::filesystem::remove(path);
}
