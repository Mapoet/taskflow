#include "agent/conversation/task_clarification_coordinator.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <thread>

namespace agent_framework::conversation {
namespace {
std::optional<TaskExecutionProfile> selected_profile(
    const TaskProfileClarification& clarification,std::string_view option_id) {
    const auto found=std::find_if(clarification.options.begin(),clarification.options.end(),
        [&](const auto& option){return option.id==option_id;});
    return found==clarification.options.end()?std::nullopt:
        std::optional<TaskExecutionProfile>(found->profile);
}
std::string retry_question(std::string_view question,std::uint32_t attempts,
                           std::uint32_t maximum) {
    return std::string(question)+" Attempts remaining: "+
        std::to_string(maximum-attempts)+".";
}
json encode_options(const std::vector<TaskClarificationOption>& options) {
    auto result=json::array();
    for(const auto& option:options)result.push_back({{"id",option.id},{"label",option.label},
        {"description",option.description}});
    return result;
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
    if(!classification.clarification||classification.clarification->options.size()<2)
        return {{}, {}, "clarification_proposal_missing"};
    value.recommended_profile=classification.profile;
    value.question=classification.clarification->question;
    value.options=classification.clarification->options;
    for(const auto& option:value.options)value.allowed_tokens.push_back(option.id);
    value.expires_at_ms=now_ms+ttl_ms;value.created_at=std::to_string(now_ms);
    value.updated_at=value.created_at;
    const auto created=clarifications_.create(value);
    if(!created.ok)return {{},{},"clarification_create_failed:"+created.error};
    const auto prompt=value.question;
    const auto options=encode_options(value.options);
    ConversationEngine engine(conversations_,[prompt,options](const auto&,const auto&){
        ModelTurnOutcome outcome;outcome.reason=ModelTurnStopReason::AwaitingInput;
        outcome.clarification=prompt;outcome.clarification_options=options;
        outcome.candidate_answer=prompt;return outcome;
    },std::move(sink));
    return engine.start_turn(request);
}

ClarificationResumeResult TaskClarificationCoordinator::answer_pending(
    const ConversationIdentity& identity, std::string_view answer,
    std::uint64_t now_ms, TurnExecutor executor, RuntimeEventSink sink) {
    ClarificationResumeResult result;
    auto pending=clarifications_.pending(identity);
    if(!pending) {
        const auto latest=clarifications_.latest(identity);
        if(latest&&latest->state==ProfileClarificationState::Confirmed&&
           latest->selected_profile==selected_profile(*latest,answer)) {
            result.handled=true;result.resumed=true;
            const auto checkpoint=conversations_.load_turn(identity,latest->turn_id);
            if(checkpoint)result.turn.checkpoint=*checkpoint;
        }
        return result;
    }
    result.handled=true;
    auto mutation=clarifications_.answer(identity,pending->clarification_id,
                                         pending->revision,answer,now_ms);
    if(!mutation.ok && (mutation.error=="clarification_revision_conflict"||
                        mutation.error=="clarification_not_pending"||
                        mutation.error.find("locked")!=std::string::npos)) {
        for(int attempt=0;attempt<100;++attempt) {
            try {
                const auto current=clarifications_.load(identity,pending->clarification_id);
                if(current&&current->state==ProfileClarificationState::Confirmed&&
                   current->selected_profile==selected_profile(*current,answer)) {
                    const auto checkpoint=conversations_.load_turn(identity,pending->turn_id);
                    if(checkpoint)result.turn.checkpoint=*checkpoint;
                    result.resumed=true;
                    return result;
                }
            } catch(const std::exception&) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    if(!mutation.ok&&mutation.state==ProfileClarificationState::Pending) {
        const auto current=clarifications_.load(identity,pending->clarification_id);
        const auto prompt=retry_question(pending->question,
            current?current->attempt_count:pending->attempt_count,
            current?current->max_attempts:pending->max_attempts);
        if(const auto checkpoint=conversations_.load_turn(identity,pending->turn_id))
            result.turn.checkpoint=*checkpoint;
        result.turn.outcome.reason=ModelTurnStopReason::AwaitingInput;
        result.turn.outcome.clarification=prompt;
        if(current)result.turn.outcome.clarification_options=encode_options(current->options);
        else result.turn.outcome.clarification_options=encode_options(pending->options);
        result.turn.outcome.candidate_answer=prompt;
        return result;
    }
    TurnRequest request;request.identity=identity;request.turn_id=pending->turn_id;
    request.task_id=pending->task_id;request.run_id=pending->run_id;
    request.classification_decision_id=pending->decision_id;
    request.clarification_id=pending->clarification_id;
    request.input=std::string(answer);request.profile=TaskExecutionProfile::Conversation;
    ConversationEngine input_engine(conversations_,{},sink);
    std::string submit_error;
    bool submitted=false;
    for(int attempt=0;attempt<100&&!submitted;++attempt) {
        submit_error.clear();
        submitted=input_engine.submit_user_input(
            request,InputDisposition::AppendToCurrentTurn,&submit_error);
        if(!submitted&&submit_error.find("locked")!=std::string::npos)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        else if(!submitted)break;
    }
    if(!submitted) {
        result.error="clarification_input_commit_failed:"+submit_error;return result;
    }
    const bool continue_execution=mutation.ok ||
        mutation.state==ProfileClarificationState::Exhausted ||
        mutation.state==ProfileClarificationState::Expired;
    if(!continue_execution) {
        auto checkpoint=conversations_.load_turn(identity,pending->turn_id);
        if(checkpoint)result.turn.checkpoint=*checkpoint;
        result.turn.outcome.reason=ModelTurnStopReason::AwaitingInput;
        result.turn.outcome.clarification=mutation.error;
        result.turn.outcome.candidate_answer=mutation.error;
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
    TaskOpenResult opened;
    for(int attempt=0;attempt<100;++attempt) {
        opened=orchestrator.open_or_resume(request,pending->task_intent);
        if(opened.ok||opened.error.find("locked")==std::string::npos)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if(!opened.ok){result.error=opened.error;return result;}
    TaskMutationResult annotated;
    for(int attempt=0;attempt<100;++attempt) {
        annotated=tasks_.annotate_run_decisions(
            identity,request.task_id,request.run_id,request.classification_decision_id,
            request.clarification_id);
        if(annotated.ok||annotated.error.find("locked")==std::string::npos)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if(!annotated.ok){result.error="task_run_decision_annotation_failed:"+
        annotated.error;return result;}
    ConversationEngine resume(conversations_,std::move(executor),std::move(sink));
    result.turn=resume.resume_turn(request,TurnContinuationReason::ClarificationAnswered);
    result.resumed=result.turn.error.empty();
    if(!result.resumed)result.error=result.turn.error;
    return result;
}

}  // namespace agent_framework::conversation
