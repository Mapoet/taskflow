#include "agent/decision/task_decision_coordinator.hpp"

#include <algorithm>

#include "agent/contracts/contract.hpp"
#include "agent/conversation/task_routing_policy.hpp"

namespace agent_framework::decision {
namespace {
using nlohmann::json;

json public_options(const std::vector<DecisionOption>& options) {
    auto value=json::array();
    for(const auto& option:options)value.push_back({{"id",option.option_id},
        {"label",option.label},{"description",option.description}});
    return value;
}

json durable_options(const std::vector<DecisionOption>& options) {
    auto value=json::array();
    for(const auto& option:options)value.push_back({{"id",option.option_id},
        {"label",option.label},{"description",option.description},
        {"semantic_patch",option.semantic_patch}});
    return value;
}

conversation::TaskExecutionProfile selected_profile(const DecisionOption& option,
    conversation::TaskExecutionProfile fallback) {
    if(const auto found=option.semantic_patch.find("compatibility_profile");
       found!=option.semantic_patch.end()&&found->is_string())
        if(const auto profile=conversation::task_execution_profile(found->get<std::string>()))
            return *profile;
    const auto effect=option.semantic_patch.value("effect_class",std::string{});
    const auto assurance=option.semantic_patch.value("assurance_tier",std::string{});
    if(assurance=="professional"||assurance=="production_certification")
        return conversation::TaskExecutionProfile::Professional;
    if(effect=="external"||effect=="destructive")
        return conversation::TaskExecutionProfile::ExternalAction;
    if(effect=="workspace_write")return conversation::TaskExecutionProfile::CodeChange;
    if(effect=="read_only")return conversation::TaskExecutionProfile::ReadOnlyAnalysis;
    return fallback;
}
}

conversation::TurnResult TaskDecisionCoordinator::begin(
    const identity::RuntimeSubject& subject,const conversation::TurnRequest& request,
    conversation::TaskInputIntent intent,const conversation::TaskClassification& classification,
    std::uint64_t now_ms,std::uint64_t ttl_ms,conversation::RuntimeEventSink sink) {
    if(!classification.clarification||classification.clarification->options.size()<2)
        return {{}, {}, "decision_proposal_missing"};
    DecisionRequest decision;decision.subject=subject;decision.decision_id=classification.decision_id;
    decision.kind=DecisionKind::TaskSemantics;
    decision.resume_payload={{"task_intent",conversation::name(intent)},
        {"fallback_profile",conversation::name(classification.profile)}};
    decision.question=classification.clarification->question;
    for(const auto& option:classification.clarification->options) {
        auto patch=option.semantic_patch;
        // Programmatic/legacy classifiers may still populate only the
        // compatibility profile. Preserve that meaning when promoting their
        // clarification into the general Decision workflow.
        if(patch.empty())
            patch["compatibility_profile"]=conversation::name(option.profile);
        decision.options.push_back({option.id,option.label,option.description,
                                    std::move(patch)});
    }
    decision.expires_at_ms=now_ms+ttl_ms;decision.created_at=std::to_string(now_ms);
    decision.updated_at=decision.created_at;
    decision.origin_digest=contracts::canonical_digest({{"decision_id",decision.decision_id},
        {"question",decision.question},{"options",durable_options(decision.options)},
        {"task_intent",conversation::name(intent)}}).value_or("");
    const auto created=decisions_.create(decision);
    if(!created.ok)return {{}, {}, "decision_create_failed:"+created.error};
    const auto prompt=decision.question;const auto options=public_options(decision.options);
    conversation::ConversationEngine engine(conversations_,[prompt,options](const auto&,const auto&){
        conversation::ModelTurnOutcome outcome;
        outcome.reason=conversation::ModelTurnStopReason::AwaitingInput;
        outcome.clarification=prompt;outcome.clarification_options=options;
        outcome.candidate_answer=prompt;return outcome;},std::move(sink));
    return engine.start_turn(request);
}

bool migrate_profile_clarification(const conversation::TaskProfileClarification& legacy,
    const identity::RuntimeSubject& base,DecisionStore& decisions,
    conversation::TaskProfileClarificationStore& clarifications,std::string* error) {
    DecisionRequest value;value.subject=base;value.subject.tenant_id=legacy.identity.tenant_id;
    value.subject.conversation_id=legacy.identity.conversation_id;
    value.subject.task_id=legacy.task_id;value.subject.run_id=legacy.run_id;
    value.subject.turn_id=legacy.turn_id;value.decision_id=legacy.clarification_id;
    value.kind=DecisionKind::TaskSemantics;
    value.resume_payload={{"task_intent",conversation::name(legacy.task_intent)},
        {"legacy_decision_id",legacy.decision_id}};
    value.question=legacy.question;value.expires_at_ms=legacy.expires_at_ms;
    value.created_at=legacy.created_at;value.updated_at=legacy.updated_at;
    for(const auto& option:legacy.options) {
        auto patch=option.semantic_patch;
        if(patch.empty())patch["compatibility_profile"]=conversation::name(option.profile);
        value.options.push_back({option.id,option.label,option.description,std::move(patch)});
        if(option.profile==legacy.recommended_profile)value.recommended_option_id=option.id;
    }
    value.origin_digest=contracts::canonical_digest({{"legacy_clarification_id",legacy.clarification_id},
        {"legacy_revision",legacy.revision},{"question",legacy.question},
        {"options",durable_options(value.options)}}).value_or("");
    const auto created=decisions.create(value);
    if(!created.ok){if(error)*error="legacy_decision_create_failed:"+created.error;return false;}
    const auto cancelled=clarifications.cancel(legacy.identity,legacy.clarification_id,legacy.revision);
    if(!cancelled.ok){if(error)*error="legacy_clarification_cancel_failed:"+cancelled.error;return false;}
    return true;
}

DecisionResumeResult TaskDecisionCoordinator::answer_pending(
    const identity::RuntimeSubject& subject,std::string_view option_id,
    std::uint64_t now_ms,conversation::TurnExecutor executor,
    conversation::RuntimeEventSink sink) {
    DecisionResumeResult result;
    auto pending=decisions_.pending(subject.tenant_id,subject.session_id,
                                    subject.conversation_id);
    if(!pending)return result;
    result.handled=true;
    const auto selected=std::find_if(pending->options.begin(),pending->options.end(),
        [&](const auto& option){return option.option_id==option_id;});
    const auto mutation=decisions_.answer(subject.tenant_id,pending->decision_id,
                                          pending->revision,option_id,now_ms);
    if(!mutation.ok) {
        if(const auto checkpoint=conversations_.load_turn(
            {subject.tenant_id,subject.conversation_id},subject.turn_id))
            result.turn.checkpoint=*checkpoint;
        result.turn.outcome.reason=conversation::ModelTurnStopReason::AwaitingInput;
        result.turn.outcome.clarification=pending->question;
        result.turn.outcome.clarification_options=public_options(pending->options);
        result.turn.outcome.candidate_answer=mutation.error;
        // An invalid user choice is normal interaction state, not a failed turn.
        // Keep the durable Decision pending and re-render its authoritative options.
        if(mutation.error!="decision_option_invalid")result.error=mutation.error;
        return result;
    }
    if(selected==pending->options.end()) {result.error="decision_option_missing";return result;}
    conversation::TurnRequest request;
    request.identity={subject.tenant_id,subject.conversation_id};
    request.turn_id=subject.turn_id;request.task_id=subject.task_id;request.run_id=subject.run_id;
    request.classification_decision_id=pending->resume_payload.value(
        "legacy_decision_id",pending->decision_id);
    request.clarification_id=pending->decision_id;request.input=std::string(option_id);
    request.profile=selected_profile(*selected,conversation::TaskExecutionProfile::Conversation);
    conversation::TaskClassification semantic;
    semantic.profile=request.profile;
    if(const auto value=selected->semantic_patch.find("intent");value!=selected->semantic_patch.end()&&value->is_string())
        semantic.intent=conversation::task_intent_kind(value->get<std::string>()).value_or(semantic.intent);
    if(const auto value=selected->semantic_patch.find("work_shape");value!=selected->semantic_patch.end()&&value->is_string())
        semantic.work_shape=conversation::work_shape(value->get<std::string>()).value_or(semantic.work_shape);
    if(const auto value=selected->semantic_patch.find("assurance_tier");value!=selected->semantic_patch.end()&&value->is_string())
        semantic.assurance_tier=conversation::assurance_tier(value->get<std::string>()).value_or(semantic.assurance_tier);
    const auto effect=selected->semantic_patch.value("effect_class",std::string("none"));
    for(auto candidate:{conversation::EffectClass::None,conversation::EffectClass::ReadOnly,
        conversation::EffectClass::WorkspaceWrite,conversation::EffectClass::External,
        conversation::EffectClass::Destructive})
        if(conversation::name(candidate)==effect)semantic.effect_class=candidate;
    const auto route=conversation::decide_task_route(semantic);
    request.work_shape=std::string(conversation::name(semantic.work_shape));
    request.effect_class=std::string(conversation::name(semantic.effect_class));
    request.assurance_tier=std::string(conversation::name(semantic.assurance_tier));
    request.promotion_mode=std::string(conversation::name(route.promotion));
    request.planning_depth=std::string(conversation::name(route.planning_depth));
    request.routing_policy_revision=route.policy_revision;
    request.promote_to_task=route.promote_to_task;
    request.planning_required=route.planning_required;
    conversation::ConversationEngine input_engine(conversations_,{});
    std::string input_error;
    if(!input_engine.submit_user_input(request,conversation::InputDisposition::AppendToCurrentTurn,
                                       &input_error)) {
        result.error="decision_input_commit_failed:"+input_error;return result;
    }
    for(const auto& message:conversations_.messages(request.identity))
        if(message.turn_id==request.turn_id&&message.role=="user") {
            request.input=message.content;break;
        }
    conversation::TaskOrchestrator orchestrator(tasks_);
    const auto resume_intent=conversation::task_input_intent(
        pending->resume_payload.value("task_intent",std::string("initial_request")))
        .value_or(conversation::TaskInputIntent::InitialRequest);
    const auto opened=orchestrator.open_or_resume(request,resume_intent);
    if(!opened.ok){result.error=opened.error;return result;}
    const auto annotated=tasks_.annotate_run_decisions(request.identity,request.task_id,
        request.run_id,request.classification_decision_id,pending->decision_id);
    if(!annotated.ok){result.error="task_run_decision_annotation_failed:"+annotated.error;return result;}
    conversation::ConversationEngine resume(conversations_,std::move(executor),std::move(sink));
    result.turn=resume.resume_turn(request,conversation::TurnContinuationReason::ClarificationAnswered);
    result.resumed=result.turn.error.empty();if(!result.resumed)result.error=result.turn.error;
    return result;
}

}  // namespace agent_framework::decision
