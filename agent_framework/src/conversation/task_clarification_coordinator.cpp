#include "agent/conversation/task_clarification_coordinator.hpp"

#include <array>

namespace agent_framework::conversation {
namespace {
std::vector<std::string> profile_tokens() {
    std::vector<std::string> result;
    for(std::size_t i=0;i<6;++i)
        result.emplace_back(name(static_cast<TaskExecutionProfile>(i)));
    return result;
}
std::string question(TaskExecutionProfile recommended) {
    return "Please confirm one execution profile exactly: conversation, "
        "read_only_analysis, artifact_delivery, code_change, external_action, "
        "professional. Recommended: " + std::string(name(recommended));
}
}

TurnResult TaskClarificationCoordinator::begin(
    const TurnRequest& request, TaskInputIntent intent,
    const TaskClassification& classification, std::uint64_t now_ms,
    std::uint64_t ttl_ms, RuntimeEventSink sink) {
    TaskProfileClarification value;
    value.identity=request.identity;value.clarification_id=classification.decision_id;
    value.task_id=request.task_id;value.decision_id=classification.decision_id;
    value.turn_id=request.turn_id;value.run_id=request.run_id;value.task_intent=intent;
    value.recommended_profile=classification.profile;value.allowed_tokens=profile_tokens();
    value.expires_at_ms=now_ms+ttl_ms;value.created_at=std::to_string(now_ms);
    value.updated_at=value.created_at;
    const auto created=clarifications_.create(value);
    if(!created.ok)return {{},{},"clarification_create_failed:"+created.error};
    const auto prompt=question(classification.profile);
    ConversationEngine engine(conversations_,[prompt](const auto&,const auto&){
        ModelTurnOutcome outcome;outcome.reason=ModelTurnStopReason::AwaitingInput;
        outcome.clarification=prompt;outcome.candidate_answer=prompt;return outcome;
    },std::move(sink));
    return engine.start_turn(request);
}

ClarificationResumeResult TaskClarificationCoordinator::answer_pending(
    const ConversationIdentity& identity, std::string_view answer,
    std::uint64_t now_ms, TurnExecutor executor, RuntimeEventSink sink) {
    ClarificationResumeResult result;
    auto pending=clarifications_.pending(identity);
    if(!pending)return result;
    result.handled=true;
    auto mutation=clarifications_.answer(identity,pending->clarification_id,
                                         pending->revision,answer,now_ms);
    TurnRequest request;request.identity=identity;request.turn_id=pending->turn_id;
    request.task_id=pending->task_id;request.run_id=pending->run_id;
    request.input=std::string(answer);request.profile=TaskExecutionProfile::Conversation;
    ConversationEngine input_engine(conversations_,{},sink);
    std::string submit_error;
    if(!input_engine.submit_user_input(request,InputDisposition::AppendToCurrentTurn,
                                       &submit_error)) {
        result.error="clarification_input_commit_failed:"+submit_error;return result;
    }
    const bool continue_execution=mutation.ok ||
        mutation.state==ProfileClarificationState::Exhausted;
    if(!continue_execution) {
        auto checkpoint=conversations_.load_turn(identity,pending->turn_id);
        if(checkpoint)result.turn.checkpoint=*checkpoint;
        result.turn.outcome.reason=ModelTurnStopReason::AwaitingInput;
        result.turn.outcome.clarification=mutation.error;
        result.turn.outcome.candidate_answer=mutation.error;
        result.error=mutation.error;
        return result;
    }
    request.profile=mutation.ok
        ? *clarifications_.load(identity,pending->clarification_id)->selected_profile
        : TaskExecutionProfile::Conversation;
    const auto messages=conversations_.messages(identity);
    for(const auto& message:messages)
        if(message.turn_id==pending->turn_id&&message.role=="user") {
            request.input=message.content;break;
        }
    TaskOrchestrator orchestrator(tasks_);
    const auto opened=orchestrator.open_or_resume(request,pending->task_intent);
    if(!opened.ok){result.error=opened.error;return result;}
    ConversationEngine resume(conversations_,std::move(executor),std::move(sink));
    result.turn=resume.resume_turn(request,TurnContinuationReason::ClarificationAnswered);
    result.resumed=result.turn.error.empty();
    if(!result.resumed)result.error=result.turn.error;
    return result;
}

}  // namespace agent_framework::conversation
