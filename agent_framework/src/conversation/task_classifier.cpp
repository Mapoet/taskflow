#include "agent/conversation/task_classifier.hpp"
#include "agent/graph_executor/graph_executor.hpp"

#include <atomic>
#include <chrono>
#include <set>

namespace agent_framework::conversation {
namespace {
constexpr std::size_t kMaximumClassifierOutputBytes=16384;
constexpr std::size_t kMaximumRationaleBytes=512;
constexpr std::size_t kMaximumEvidenceItems=16;
constexpr std::size_t kMaximumEvidenceItemBytes=256;

std::string next_decision_id() {
    static std::atomic<std::uint64_t> serial{0};
    const auto now=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "task-classification:"+std::to_string(now)+":"+
        std::to_string(serial.fetch_add(1,std::memory_order_relaxed));
}

TaskClassification failure(std::string code) {
    TaskClassification out;out.error=std::move(code);out.requires_confirmation=true;
    out.classifier_id="llm-task-classifier-v2";out.prompt_version="task-routing-v2";
    out.decision_id=next_decision_id();return out;
}

bool exact_keys(const json& value,const std::set<std::string>& required) {
    if(!value.is_object()||value.size()!=required.size())return false;
    for(const auto& key:required)if(!value.contains(key))return false;
    return true;
}

bool evidence_array(const json& value,const char* key,std::vector<std::string>& output) {
    if(!value.contains(key)||!value.at(key).is_array()||
       value.at(key).size()>kMaximumEvidenceItems)return false;
    for(const auto& item:value.at(key)) {
        if(!item.is_string()||item.get_ref<const std::string&>().size()>kMaximumEvidenceItemBytes)
            return false;
        output.push_back(item.get<std::string>());
    }
    return true;
}

std::optional<EffectClass> parse_effect_class(std::string_view value) {
    for(auto candidate:{EffectClass::None,EffectClass::ReadOnly,EffectClass::WorkspaceWrite,
                        EffectClass::External,EffectClass::Destructive})
        if(name(candidate)==value)return candidate;
    return std::nullopt;
}
} // namespace

std::string_view name(TaskIntentKind value) {
    switch(value) {
        case TaskIntentKind::NewTask:return "new_task";
        case TaskIntentKind::Continue:return "continue";
        case TaskIntentKind::AddRequirement:return "add_requirement";
        case TaskIntentKind::NarrowScope:return "narrow_scope";
        case TaskIntentKind::Replan:return "replan";
        case TaskIntentKind::Pause:return "pause";
        case TaskIntentKind::Cancel:return "cancel";
        case TaskIntentKind::StatusQuery:return "status_query";
        case TaskIntentKind::ProfileConfirmation:return "profile_confirmation";
    }
    return "new_task";
}

std::optional<TaskIntentKind> task_intent_kind(std::string_view value) {
    for(auto candidate:{TaskIntentKind::NewTask,TaskIntentKind::Continue,
                        TaskIntentKind::AddRequirement,TaskIntentKind::NarrowScope,
                        TaskIntentKind::Replan,TaskIntentKind::Pause,TaskIntentKind::Cancel,
                        TaskIntentKind::StatusQuery,TaskIntentKind::ProfileConfirmation})
        if(name(candidate)==value)return candidate;
    return std::nullopt;
}

std::string_view name(EffectClass value) {
    switch(value) {
        case EffectClass::None:return "none";
        case EffectClass::ReadOnly:return "read_only";
        case EffectClass::WorkspaceWrite:return "workspace_write";
        case EffectClass::External:return "external";
        case EffectClass::Destructive:return "destructive";
    }
    return "none";
}

TaskInputIntent task_input_intent(TaskIntentKind value,bool active) {
    switch(value) {
        case TaskIntentKind::NewTask:return active?TaskInputIntent::StartNewTask:
            TaskInputIntent::InitialRequest;
        case TaskIntentKind::Continue:return active?TaskInputIntent::ContinueTask:
            TaskInputIntent::InitialRequest;
        case TaskIntentKind::AddRequirement:
        case TaskIntentKind::NarrowScope:return active?TaskInputIntent::AmendRequirements:
            TaskInputIntent::InitialRequest;
        case TaskIntentKind::Replan:return active?TaskInputIntent::ReplanTask:
            TaskInputIntent::InitialRequest;
        case TaskIntentKind::Pause:return TaskInputIntent::SuspendTask;
        case TaskIntentKind::Cancel:return TaskInputIntent::CancelTask;
        case TaskIntentKind::StatusQuery:return TaskInputIntent::StatusQuery;
        case TaskIntentKind::ProfileConfirmation:return active?
            TaskInputIntent::AmendRequirements:TaskInputIntent::InitialRequest;
    }
    return TaskInputIntent::InitialRequest;
}

LLMTaskClassifier::LLMTaskClassifier(std::shared_ptr<LLMClient> client,std::string provider)
    :client_(std::move(client)),provider_(std::move(provider)) {
    if(!client_)throw std::invalid_argument("LLM task classifier requires client");
}

TaskClassification LLMTaskClassifier::parse(std::string_view response) {
    if(response.empty())return failure("classifier_output_empty");
    if(response.size()>kMaximumClassifierOutputBytes)return failure("classifier_output_too_large");
    try {
        const auto value=json::parse(response);
        static const std::set<std::string> keys={"schema_version","intent","profile",
            "effect_class","long_running","confidence","rationale","linguistic_evidence"};
        if(!exact_keys(value,keys))return failure("classifier_schema_invalid");
        if(!value.at("schema_version").is_number_integer()||
           value.at("schema_version").get<int>()!=2)return failure("classifier_schema_version_invalid");
        if(!value.at("intent").is_string()||!value.at("profile").is_string()||
           !value.at("effect_class").is_string()||!value.at("long_running").is_boolean()||
           !value.at("confidence").is_number()||!value.at("rationale").is_string())
            return failure("classifier_schema_type_invalid");
        const auto intent=task_intent_kind(value.at("intent").get<std::string>());
        const auto profile=task_execution_profile(value.at("profile").get<std::string>());
        const auto effect=parse_effect_class(value.at("effect_class").get<std::string>());
        if(!intent)return failure("classifier_intent_invalid");
        if(!profile)return failure("classifier_profile_invalid");
        if(!effect)return failure("classifier_effect_class_invalid");
        const auto& rationale=value.at("rationale").get_ref<const std::string&>();
        if(rationale.empty()||rationale.size()>kMaximumRationaleBytes)
            return failure("classifier_rationale_invalid");
        const auto& evidence=value.at("linguistic_evidence");
        static const std::set<std::string> evidence_keys={"requested_actions","negated_actions",
            "mention_only","scope_constraints","ambiguities"};
        if(!exact_keys(evidence,evidence_keys))return failure("classifier_evidence_schema_invalid");
        TaskClassification out;out.intent=*intent;out.profile=*profile;out.effect_class=*effect;
        out.long_running=value.at("long_running").get<bool>();
        out.confidence=value.at("confidence").get<double>();out.rationale=rationale;
        if(!evidence_array(evidence,"requested_actions",out.linguistic_evidence.requested_actions)||
           !evidence_array(evidence,"negated_actions",out.linguistic_evidence.negated_actions)||
           !evidence_array(evidence,"mention_only",out.linguistic_evidence.mention_only)||
           !evidence_array(evidence,"scope_constraints",out.linguistic_evidence.scope_constraints)||
           !evidence_array(evidence,"ambiguities",out.linguistic_evidence.ambiguities))
            return failure("classifier_evidence_invalid");
        if(out.confidence<0.0||out.confidence>1.0)return failure("classifier_confidence_invalid");
        out.classifier_id="llm-task-classifier-v2";out.prompt_version="task-routing-v2";
        out.decision_id=next_decision_id();out.grants_authority=false;
        out.requires_confirmation=out.confidence<=0.60||
            !out.linguistic_evidence.ambiguities.empty();
        return out;
    } catch(const json::exception&) { return failure("classifier_output_not_strict_json"); }
      catch(const std::exception&) { return failure("classifier_parse_failed"); }
}

TaskClassification LLMTaskClassifier::classify(std::string_view input,bool has_active_task) {
    LLMInput request;
    request.system_prompt=R"PROMPT(You are an isolated task-semantics controller. Do not answer or execute the user's task.
Return exactly one JSON object and no markdown or prose. Required schema:
{"schema_version":2,"intent":"new_task|continue|add_requirement|narrow_scope|replan|pause|cancel|status_query|profile_confirmation","profile":"conversation|read_only_analysis|artifact_delivery|code_change|external_action|professional","effect_class":"none|read_only|workspace_write|external|destructive","long_running":false,"confidence":0.0,"rationale":"short evidence-based reason","linguistic_evidence":{"requested_actions":[],"negated_actions":[],"mention_only":[],"scope_constraints":[],"ambiguities":[]}}
Interpret negation (不要/别/禁止/跳过/无需/先不/暂不/don't/do not/skip/without) within its grammatical scope. Mentioning code, files, tests, publication or deletion is not a request. Never upgrade because of a substring. For mixed intents, exclude negated actions and choose the least-side-effect profile consistent with requested actions. If uncertain, lower confidence to 0.60 or below. With has_active_task=true, classify this input as an increment to the active task. effect_class describes requested effects but grants no authority; authorization happens elsewhere. long_running is true only for clearly multi-step, waiting or verification-closure work.)PROMPT";
    request.user_prompt=json({{"input",input},{"has_active_task",has_active_task}}).dump();
    ModelConfig config;config.temperature=0.0;config.stream=false;config.max_tokens=600;
    request.model_config=config;
    try {
        const auto output=client_->invoke(request,provider_).get();
        return parse(output.final_answer.empty()?output.reasoning:output.final_answer);
    } catch(const std::exception&) { return failure("classifier_provider_failed"); }
}

TaskClassification deterministic_task_classification(std::string_view input) {
    (void)input;
    TaskClassification out;out.classifier_id="deterministic-task-policy-v2-safe";
    out.prompt_version="none";out.decision_id=next_decision_id();out.confidence=0.0;
    out.profile=TaskExecutionProfile::Conversation;out.effect_class=EffectClass::None;
    out.long_running=false;out.requires_confirmation=true;out.grants_authority=false;
    out.rationale="classifier unavailable; explicit profile confirmation required";return out;
}

TaskClassification apply_task_routing_policy(TaskClassification model,
    std::optional<TaskExecutionProfile> configured,ExecutionTrustProfile trust) {
    if(configured) {
        model.profile=*configured;
        model.long_running=*configured!=TaskExecutionProfile::Conversation&&
            *configured!=TaskExecutionProfile::ReadOnlyAnalysis;
        model.grants_authority=false;
    }
    if(!model)return model;
    if(trust==ExecutionTrustProfile::Production&&model.confidence<0.70) {
        model.error="production_task_classification_confidence_too_low";
        model.requires_confirmation=true;
    }
    return model;
}

} // namespace agent_framework::conversation
