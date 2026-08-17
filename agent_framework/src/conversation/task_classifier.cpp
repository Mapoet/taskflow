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
constexpr std::size_t kMaximumClarificationOptions=5;

std::string next_decision_id() {
    static std::atomic<std::uint64_t> serial{0};
    const auto now=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "task-classification:"+std::to_string(now)+":"+
        std::to_string(serial.fetch_add(1,std::memory_order_relaxed));
}

TaskClassification failure(std::string code) {
    TaskClassification out;out.error=std::move(code);out.requires_confirmation=true;
    out.classifier_id="llm-task-classifier-v4";out.prompt_version="task-routing-v4";
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

TaskExecutionProfile compatibility_profile(WorkShape shape,EffectClass effect,
                                            AssuranceTier assurance) {
    if(assurance==AssuranceTier::Professional ||
       assurance==AssuranceTier::ProductionCertification)
        return TaskExecutionProfile::Professional;
    if(effect==EffectClass::Destructive || effect==EffectClass::External)
        return TaskExecutionProfile::ExternalAction;
    if(effect==EffectClass::WorkspaceWrite)
        return TaskExecutionProfile::CodeChange;
    if(shape==WorkShape::LongRunningTask || shape==WorkShape::ContinuousTask ||
       effect==EffectClass::ReadOnly)
        return TaskExecutionProfile::ReadOnlyAnalysis;
    return TaskExecutionProfile::Conversation;
}
} // namespace

std::string_view name(WorkShape value) {
    switch(value) {
        case WorkShape::SingleTurn:return "single_turn";
        case WorkShape::BoundedTask:return "bounded_task";
        case WorkShape::LongRunningTask:return "long_running_task";
        case WorkShape::ContinuousTask:return "continuous_task";
    }
    return "single_turn";
}

std::optional<WorkShape> work_shape(std::string_view value) {
    for(auto candidate:{WorkShape::SingleTurn,WorkShape::BoundedTask,
                        WorkShape::LongRunningTask,WorkShape::ContinuousTask})
        if(name(candidate)==value)return candidate;
    return std::nullopt;
}

std::string_view name(AssuranceTier value) {
    switch(value) {
        case AssuranceTier::Basic:return "basic";
        case AssuranceTier::Functional:return "functional";
        case AssuranceTier::Professional:return "professional";
        case AssuranceTier::ProductionCertification:return "production_certification";
    }
    return "basic";
}

std::optional<AssuranceTier> assurance_tier(std::string_view value) {
    for(auto candidate:{AssuranceTier::Basic,AssuranceTier::Functional,
                        AssuranceTier::Professional,
                        AssuranceTier::ProductionCertification})
        if(name(candidate)==value)return candidate;
    return std::nullopt;
}

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

LLMTaskClassifier::LLMTaskClassifier(std::shared_ptr<LLMClient> client,std::string provider,
    TaskClassifierObserver observer)
    :client_(std::move(client)),provider_(std::move(provider)),observer_(std::move(observer)) {
    if(!client_)throw std::invalid_argument("LLM task classifier requires client");
}

TaskClassification LLMTaskClassifier::parse(std::string_view response) {
    if(response.empty())return failure("classifier_output_empty");
    if(response.size()>kMaximumClassifierOutputBytes)return failure("classifier_output_too_large");
    try {
        const auto value=json::parse(response);
        if(!value.is_object()||!value.contains("schema_version")||
           !value.at("schema_version").is_number_integer())
            return failure("classifier_schema_version_invalid");
        const auto schema=value.at("schema_version").get<int>();
        static const std::set<std::string> v3_keys={"schema_version","intent","profile",
            "effect_class","long_running","confidence","rationale","linguistic_evidence",
            "clarification"};
        static const std::set<std::string> v4_keys={"schema_version","intent","work_shape",
            "effect_class","assurance_tier","confidence","rationale","linguistic_evidence",
            "clarification"};
        if(schema!=3&&schema!=4)return failure("classifier_schema_version_invalid");
        if(!exact_keys(value,schema==4?v4_keys:v3_keys))
            return failure("classifier_schema_invalid");
        if(!value.at("intent").is_string()||!value.at("effect_class").is_string()||
           !value.at("confidence").is_number()||!value.at("rationale").is_string())
            return failure("classifier_schema_type_invalid");
        const auto intent=task_intent_kind(value.at("intent").get<std::string>());
        const auto effect=parse_effect_class(value.at("effect_class").get<std::string>());
        if(!intent)return failure("classifier_intent_invalid");
        if(!effect)return failure("classifier_effect_class_invalid");
        std::optional<TaskExecutionProfile> profile;
        std::optional<WorkShape> shape;
        std::optional<AssuranceTier> assurance;
        if(schema==3) {
            if(!value.at("profile").is_string()||!value.at("long_running").is_boolean())
                return failure("classifier_schema_type_invalid");
            profile=task_execution_profile(value.at("profile").get<std::string>());
            if(!profile)return failure("classifier_profile_invalid");
            shape=value.at("long_running").get<bool>()
                ?WorkShape::LongRunningTask:WorkShape::SingleTurn;
            assurance=*profile==TaskExecutionProfile::Professional
                ?AssuranceTier::Professional:AssuranceTier::Basic;
        } else {
            if(!value.at("work_shape").is_string()||
               !value.at("assurance_tier").is_string())
                return failure("classifier_schema_type_invalid");
            shape=work_shape(value.at("work_shape").get<std::string>());
            assurance=assurance_tier(value.at("assurance_tier").get<std::string>());
            if(!shape)return failure("classifier_work_shape_invalid");
            if(!assurance)return failure("classifier_assurance_tier_invalid");
            profile=compatibility_profile(*shape,*effect,*assurance);
        }
        const auto& rationale=value.at("rationale").get_ref<const std::string&>();
        if(rationale.empty()||rationale.size()>kMaximumRationaleBytes)
            return failure("classifier_rationale_invalid");
        const auto& evidence=value.at("linguistic_evidence");
        static const std::set<std::string> evidence_keys={"requested_actions","negated_actions",
            "mention_only","scope_constraints","ambiguities"};
        if(!exact_keys(evidence,evidence_keys))return failure("classifier_evidence_schema_invalid");
        TaskClassification out;out.schema_version=schema;out.intent=*intent;
        out.work_shape=*shape;out.assurance_tier=*assurance;
        out.profile=*profile;out.effect_class=*effect;
        out.long_running=*shape==WorkShape::LongRunningTask||
                         *shape==WorkShape::ContinuousTask;
        out.confidence=value.at("confidence").get<double>();out.rationale=rationale;
        if(!evidence_array(evidence,"requested_actions",out.linguistic_evidence.requested_actions)||
           !evidence_array(evidence,"negated_actions",out.linguistic_evidence.negated_actions)||
           !evidence_array(evidence,"mention_only",out.linguistic_evidence.mention_only)||
           !evidence_array(evidence,"scope_constraints",out.linguistic_evidence.scope_constraints)||
           !evidence_array(evidence,"ambiguities",out.linguistic_evidence.ambiguities))
            return failure("classifier_evidence_invalid");
        if(out.confidence<0.0||out.confidence>1.0)return failure("classifier_confidence_invalid");
        if(!value.at("clarification").is_null()) {
            const auto& clarification=value.at("clarification");
            static const std::set<std::string> clarification_keys={"question","options"};
            if(!exact_keys(clarification,clarification_keys)||
               !clarification.at("question").is_string()||
               clarification.at("question").get_ref<const std::string&>().empty()||
               clarification.at("question").get_ref<const std::string&>().size()>512||
               !clarification.at("options").is_array()||clarification.at("options").size()<2||
               clarification.at("options").size()>kMaximumClarificationOptions)
                return failure("classifier_clarification_invalid");
            TaskClarificationProposal proposal;
            proposal.question=clarification.at("question").get<std::string>();
            std::set<std::string> ids;
            for(const auto& item:clarification.at("options")) {
                static const std::set<std::string> option_v3_keys={"id","label","description","profile"};
                static const std::set<std::string> option_v4_keys={"id","label","description","semantic_patch"};
                if(!exact_keys(item,schema==4?option_v4_keys:option_v3_keys)||
                   !item.at("id").is_string()||
                   !item.at("label").is_string()||!item.at("description").is_string()||
                   (schema==3&&!item.at("profile").is_string())||
                   (schema==4&&!item.at("semantic_patch").is_object()))
                    return failure("classifier_clarification_option_invalid");
                TaskClarificationOption option;
                option.id=item.at("id").get<std::string>();
                option.label=item.at("label").get<std::string>();
                option.description=item.at("description").get<std::string>();
                std::optional<TaskExecutionProfile> option_profile;
                if(schema==3)
                    option_profile=task_execution_profile(item.at("profile").get<std::string>());
                else {
                    option.semantic_patch=item.at("semantic_patch");
                    if(option.semantic_patch.contains("compatibility_profile")&&
                       option.semantic_patch.at("compatibility_profile").is_string())
                        option_profile=task_execution_profile(
                            option.semantic_patch.at("compatibility_profile").get<std::string>());
                    if(!option_profile)option_profile=out.profile;
                }
                if(option.id.empty()||option.id.size()>64||option.label.empty()||
                   option.label.size()>128||option.description.size()>256||!option_profile||
                   !ids.insert(option.id).second)return failure("classifier_clarification_option_invalid");
                option.profile=*option_profile;proposal.options.push_back(std::move(option));
            }
            out.clarification=std::move(proposal);
        }
        out.classifier_id=schema==4?"llm-task-classifier-v4":"llm-task-classifier-v3";
        out.prompt_version=schema==4?"task-routing-v4":"task-routing-v3";
        out.decision_id=next_decision_id();out.grants_authority=false;
        const bool materially_ambiguous=out.confidence<=0.60||
            !out.linguistic_evidence.ambiguities.empty();
        out.requires_confirmation=materially_ambiguous&&out.clarification.has_value();
        return out;
    } catch(const json::exception&) { return failure("classifier_output_not_strict_json"); }
      catch(const std::exception&) { return failure("classifier_parse_failed"); }
}

TaskClassification LLMTaskClassifier::classify(std::string_view input,bool has_active_task) {
    return classify_observed(input,has_active_task,{});
}

TaskClassification LLMTaskClassifier::classify_observed(std::string_view input,
    bool has_active_task,TaskClassifierObserver call_observer) {
    LLMInput request;
    request.system_prompt=R"PROMPT(You are an isolated task-semantics controller. Do not answer or execute the user's task.
Return exactly one JSON object and no markdown or prose. Required schema:
{"schema_version":4,"intent":"new_task|continue|add_requirement|narrow_scope|replan|pause|cancel|status_query|profile_confirmation","work_shape":"single_turn|bounded_task|long_running_task|continuous_task","effect_class":"none|read_only|workspace_write|external|destructive","assurance_tier":"basic|functional|professional|production_certification","confidence":0.0,"rationale":"short evidence-based reason","linguistic_evidence":{"requested_actions":[],"negated_actions":[],"mention_only":[],"scope_constraints":[],"ambiguities":[]},"clarification":null}
Interpret negation (不要/别/禁止/跳过/无需/先不/暂不/don't/do not/skip/without) within its grammatical scope. Mentioning code, files, tests, publication or deletion is not a request. Never upgrade because of a substring. For mixed intents, exclude negated actions and choose the least-side-effect semantics consistent with requested actions. With has_active_task=true, classify this input as an increment to the active task. effect_class describes requested effects but grants no authority; authorization happens elsewhere.
work_shape describes duration and orchestration independently from effect_class. A deep read-only research task can be long_running_task. A small code edit can be bounded_task. assurance_tier describes verification depth independently from both.
Set clarification to null whenever the task can be routed safely. Only for a material ambiguity that changes the intended outcome, set confidence to 0.60 or below and provide a concise user-facing question with 2-5 mutually exclusive options. Each option has id, label, description and a semantic_patch object containing only fields from this semantic schema. Option ids are opaque stable identifiers; labels and descriptions must describe the user's choices, never internal execution-profile names.)PROMPT";
    request.user_prompt=json({{"input",input},{"has_active_task",has_active_task}}).dump();
    ModelConfig config;config.temperature=0.0;config.stream=false;config.max_tokens=600;
    request.model_config=config;
    const auto started=std::chrono::steady_clock::now();
    TaskClassifierInvocation observation;observation.invocation_id=next_decision_id();
    observation.provider=provider_;observation.classifier_id="llm-task-classifier-v4";
    observation.model=client_->get_model_name(provider_);
    observation.prompt_revision="task-routing-v4";
    observation.input_digest=contracts::canonical_digest({{"input",input},
        {"has_active_task",has_active_task}}).value_or("");
    try {
        const auto output=client_->invoke(request,provider_).get();
        if(output.usage) {
            observation.input_tokens=output.usage->input_tokens;
            observation.output_tokens=output.usage->output_tokens;
            observation.cached_input_tokens=output.usage->cached_input_tokens;
            observation.cost_usd=output.usage->cost_usd;
        }
        const auto raw=output.final_answer.empty()?output.reasoning:output.final_answer;
        auto result=parse(raw);observation.output_digest=contracts::canonical_digest({{"output",raw}}).value_or("");
        observation.confidence=result.confidence;observation.outcome=result?"parsed":"malformed";
        observation.error_code=result.error;
        observation.latency_ms=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-started).count();
        if(observer_)observer_(observation);
        if(call_observer)call_observer(observation);
        return result;
    } catch(const std::exception&) {
        observation.outcome="provider_error";observation.error_code="classifier_provider_failed";
        observation.fallback_used=true;observation.latency_ms=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-started).count();
        if(observer_)observer_(observation);
        if(call_observer)call_observer(observation);
        return failure("classifier_provider_failed"); }
}

TaskClassification deterministic_task_classification(std::string_view input) {
    (void)input;
    TaskClassification out;out.classifier_id="deterministic-task-policy-v3-safe";
    out.prompt_version="none";out.decision_id=next_decision_id();out.confidence=0.0;
    out.profile=TaskExecutionProfile::Conversation;out.effect_class=EffectClass::None;
    out.work_shape=WorkShape::SingleTurn;out.assurance_tier=AssuranceTier::Basic;
    out.long_running=false;out.requires_confirmation=false;out.grants_authority=false;
    out.rationale="classifier unavailable; conservative conversation routing";return out;
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
        if(!model.clarification) {
            model.profile=TaskExecutionProfile::Conversation;
            model.effect_class=EffectClass::None;
            model.long_running=false;
            model.requires_confirmation=false;
            model.rationale+="; production conservative routing applied";
        }
    }
    return model;
}

void TaskSemanticCalibrationMetrics::observe(const TaskClassification& actual,
    const TaskSemanticCalibrationSample& expected) {
    const auto effect_rank=[](EffectClass value) {
        switch(value) {
        case EffectClass::None:return 0;
        case EffectClass::ReadOnly:return 1;
        case EffectClass::WorkspaceWrite:return 2;
        case EffectClass::External:return 3;
        case EffectClass::Destructive:return 4;
        }
        return 0;
    };
    samples_.fetch_add(1,std::memory_order_relaxed);
    if(effect_rank(actual.effect_class)>effect_rank(expected.expected_effect))
        false_high_effect_.fetch_add(1,std::memory_order_relaxed);
    if(expected.expected_planning&&!expected.actual_planning)
        missed_planning_.fetch_add(1,std::memory_order_relaxed);
    if(!expected.expected_planning&&expected.actual_planning)
        unnecessary_planning_.fetch_add(1,std::memory_order_relaxed);
    if(!expected.expected_clarification&&actual.requires_confirmation)
        unnecessary_clarification_.fetch_add(1,std::memory_order_relaxed);
    if(expected.decision_abandoned)
        decision_abandonment_.fetch_add(1,std::memory_order_relaxed);
}

nlohmann::json TaskSemanticCalibrationMetrics::snapshot() const {
    const auto samples=samples_.load(std::memory_order_relaxed);
    const auto metric=[samples](std::uint64_t count) {
        return nlohmann::json{{"count",count},{"rate",samples?
            static_cast<double>(count)/static_cast<double>(samples):0.0}};
    };
    nlohmann::json out{{"schema","agent.task_semantic_calibration/v1"},
        {"samples",samples},
        {"false_high_effect",metric(false_high_effect_.load(std::memory_order_relaxed))},
        {"missed_planning",metric(missed_planning_.load(std::memory_order_relaxed))},
        {"unnecessary_planning",metric(unnecessary_planning_.load(std::memory_order_relaxed))},
        {"unnecessary_clarification",metric(unnecessary_clarification_.load(std::memory_order_relaxed))},
        {"decision_abandonment",metric(decision_abandonment_.load(std::memory_order_relaxed))}};
    out["canonical_digest"]=contracts::canonical_digest(out).value_or("");
    return out;
}

} // namespace agent_framework::conversation
