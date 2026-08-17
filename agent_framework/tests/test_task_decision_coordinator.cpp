#include <cassert>
#include <filesystem>

#include "agent/decision/task_decision_coordinator.hpp"

using namespace agent_framework;

int main() {
    const auto path=(std::filesystem::temp_directory_path()/"agent-task-decision.sqlite").string();
    std::filesystem::remove(path);
    auto subject=identity::legacy_local_subject("session","conversation");
    subject.task_id="task";subject.run_id="run";subject.turn_id="turn";
    conversation::TurnRequest request{{"local","conversation"},"turn","research or implement",
        conversation::TaskExecutionProfile::Conversation,5};
    request.task_id="task";request.run_id="run";
    conversation::TaskClassification classification;
    classification.schema_version=4;classification.decision_id="decision";
    classification.profile=conversation::TaskExecutionProfile::ReadOnlyAnalysis;
    classification.requires_confirmation=true;
    classification.clarification=conversation::TaskClarificationProposal{
        "Research only or implement?",{
            {"research","Research only","No write",conversation::TaskExecutionProfile::ReadOnlyAnalysis,
             {{"effect_class","read_only"},{"work_shape","long_running_task"}}},
            {"implement","Implement","Write workspace",conversation::TaskExecutionProfile::CodeChange,
             {{"effect_class","workspace_write"},{"work_shape","bounded_task"}}}}};
    {
        conversation::SQLiteConversationStore conversations(path);
        conversation::SQLiteTaskRegistry tasks(path);decision::SQLiteDecisionStore decisions(path);
        decision::TaskDecisionCoordinator coordinator(conversations,decisions,tasks);
        auto waiting=coordinator.begin(subject,request,conversation::TaskInputIntent::InitialRequest,
                                       classification,100,1000);
        assert(waiting.error.empty()&&waiting.checkpoint.phase==conversation::TurnPhase::AwaitingInput);
        assert(!tasks.active(request.identity));
    }
    {
        const auto migration_path=path+".migration";std::filesystem::remove(migration_path);
        conversation::SQLiteTaskProfileClarificationStore legacy_store(migration_path);
        decision::SQLiteDecisionStore decision_store(migration_path);
        conversation::TaskProfileClarification legacy;legacy.identity={"local","conversation"};
        legacy.clarification_id="legacy-question";legacy.decision_id="legacy-classifier";
        legacy.task_id="legacy-task";legacy.run_id="legacy-run";legacy.turn_id="legacy-turn";
        legacy.question="Read or write?";legacy.expires_at_ms=1000;legacy.created_at="1";legacy.updated_at="1";
        legacy.options={{"read","Read","Inspect",conversation::TaskExecutionProfile::ReadOnlyAnalysis,{}},
                        {"write","Write","Edit",conversation::TaskExecutionProfile::CodeChange,{}}};
        legacy.allowed_tokens={"read","write"};assert(legacy_store.create(legacy).ok);
        std::string error;assert(decision::migrate_profile_clarification(legacy,subject,
            decision_store,legacy_store,&error));assert(error.empty());
        assert(!legacy_store.pending(legacy.identity));
        auto migrated=decision_store.pending("local","session","conversation");
        assert(migrated&&migrated->resume_payload.at("legacy_decision_id")=="legacy-classifier");
        std::filesystem::remove(migration_path);
    }
    {
        conversation::SQLiteConversationStore conversations(path);
        conversation::SQLiteTaskRegistry tasks(path);decision::SQLiteDecisionStore decisions(path);
        decision::TaskDecisionCoordinator coordinator(conversations,decisions,tasks);
        auto invalid=coordinator.answer_pending(subject,"not-an-option",150,{});
        assert(invalid.handled&&!invalid.resumed&&invalid.error.empty());
        assert(invalid.turn.outcome.reason==conversation::ModelTurnStopReason::AwaitingInput);
        int calls=0;auto resumed=coordinator.answer_pending(subject,"implement",200,
            [&](const conversation::TurnRequest& restored,const conversation::TurnCheckpoint&){
                ++calls;assert(restored.input=="research or implement");
                assert(restored.profile==conversation::TaskExecutionProfile::CodeChange);
                assert(restored.promotion_mode=="bounded_task"&&restored.planning_required);
                conversation::ModelTurnOutcome output;output.reason=conversation::ModelTurnStopReason::EndTurn;
                output.candidate_answer="implemented";return output;});
        assert(resumed.handled&&resumed.resumed&&resumed.error.empty()&&calls==1);
        auto task=tasks.active(request.identity);assert(task&&task->task_id=="task");
        auto runs=tasks.runs(request.identity,"task");assert(runs.size()==1&&
            runs.front().classification_decision_id=="decision");
    }
    std::filesystem::remove(path);
}
