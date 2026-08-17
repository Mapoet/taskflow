#include <cassert>

#include "agent/conversation/task_classifier.hpp"

namespace {
nlohmann::json valid() {
    return {{"schema_version",2},{"intent","new_task"},{"profile","code_change"},
        {"effect_class","workspace_write"},{"long_running",true},{"confidence",0.93},
        {"rationale","explicit repository modification"},
        {"linguistic_evidence",{{"requested_actions",nlohmann::json::array({"modify repository"})},
            {"negated_actions",nlohmann::json::array()},{"mention_only",nlohmann::json::array()},
            {"scope_constraints",nlohmann::json::array()},{"ambiguities",nlohmann::json::array()}}}};
}
}

int main() {
    using namespace agent_framework;
    using namespace agent_framework::conversation;
    auto parsed=LLMTaskClassifier::parse(valid().dump());
    assert(parsed&&parsed.schema_version==2&&parsed.intent==TaskIntentKind::NewTask);
    assert(parsed.profile==TaskExecutionProfile::CodeChange&&parsed.long_running);
    assert(parsed.effect_class==EffectClass::WorkspaceWrite&&parsed.confidence==0.93);
    assert(parsed.classifier_id=="llm-task-classifier-v2"&&
           parsed.prompt_version=="task-routing-v2"&&!parsed.grants_authority);
    assert(task_input_intent(TaskIntentKind::Continue,true)==TaskInputIntent::ContinueTask);
    assert(task_input_intent(TaskIntentKind::NewTask,true)==TaskInputIntent::StartNewTask);
    assert(task_input_intent(TaskIntentKind::NarrowScope,true)==TaskInputIntent::AmendRequirements);

    assert(!LLMTaskClassifier::parse("not-json"));
    assert(!LLMTaskClassifier::parse("```json\n"+valid().dump()+"\n```"));
    assert(!LLMTaskClassifier::parse(valid().dump()+valid().dump()));
    auto missing=valid();missing.erase("intent");assert(!LLMTaskClassifier::parse(missing.dump()));
    auto unknown=valid();unknown["extra"]=true;assert(!LLMTaskClassifier::parse(unknown.dump()));
    auto spoof=valid();spoof["classifier_id"]="model-spoof";
    assert(!LLMTaskClassifier::parse(spoof.dump()));
    auto bad_confidence=valid();bad_confidence["confidence"]=1.1;
    assert(!LLMTaskClassifier::parse(bad_confidence.dump()));
    auto uncertain=valid();uncertain["confidence"]=0.55;
    assert(LLMTaskClassifier::parse(uncertain.dump()).requires_confirmation);
    auto ambiguous=valid();ambiguous["linguistic_evidence"]["ambiguities"]={"unclear target"};
    assert(LLMTaskClassifier::parse(ambiguous.dump()).requires_confirmation);
    auto bad_evidence=valid();bad_evidence["linguistic_evidence"]["ambiguities"]="none";
    assert(!LLMTaskClassifier::parse(bad_evidence.dump()));
    auto too_large=valid();too_large["rationale"]=std::string(513,'x');
    assert(!LLMTaskClassifier::parse(too_large.dump()));

    // Deterministic fallback is observation-only and can never escalate authority/profile.
    for(const char* input:{"不要发布，先讨论方案","生成分析报告和图片文件",
                           "把登录接口实现并补测试","delete production data"}) {
        auto safe=deterministic_task_classification(input);
        assert(safe&&safe.profile==TaskExecutionProfile::Conversation&&!safe.long_running);
        assert(safe.requires_confirmation&&!safe.grants_authority&&safe.confidence==0.0);
    }

    auto configured=apply_task_routing_policy(parsed,TaskExecutionProfile::Professional,
                                               ExecutionTrustProfile::Production);
    assert(configured&&configured.profile==TaskExecutionProfile::Professional&&
           configured.confidence==0.93&&!configured.grants_authority);
    auto explicit_conversation=apply_task_routing_policy(parsed,
        TaskExecutionProfile::Conversation,ExecutionTrustProfile::Production);
    assert(explicit_conversation&&explicit_conversation.profile==TaskExecutionProfile::Conversation&&
           explicit_conversation.confidence==0.93);
    TaskClassification invalid;invalid.error="classifier_failed";
    auto invalid_with_profile=apply_task_routing_policy(invalid,TaskExecutionProfile::Professional,
        ExecutionTrustProfile::Production);
    assert(!invalid_with_profile); // profile override never replaces intent cognition
    auto weak=deterministic_task_classification("分析问题");
    weak=apply_task_routing_policy(weak,std::nullopt,ExecutionTrustProfile::Production);
    assert(!weak&&weak.error=="production_task_classification_confidence_too_low"&&
           weak.requires_confirmation);
}
