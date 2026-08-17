#include <cassert>

#include "agent/conversation/task_classifier.hpp"

namespace {
nlohmann::json valid() {
    return {{"schema_version",3},{"intent","new_task"},{"profile","code_change"},
        {"effect_class","workspace_write"},{"long_running",true},{"confidence",0.93},
        {"rationale","explicit repository modification"},
        {"linguistic_evidence",{{"requested_actions",nlohmann::json::array({"modify repository"})},
            {"negated_actions",nlohmann::json::array()},{"mention_only",nlohmann::json::array()},
            {"scope_constraints",nlohmann::json::array()},{"ambiguities",nlohmann::json::array()}}},
        {"clarification",nullptr}};
}
nlohmann::json valid_v4() {
    return {{"schema_version",4},{"intent","new_task"},{"work_shape","long_running_task"},
        {"effect_class","read_only"},{"assurance_tier","professional"},{"confidence",0.91},
        {"rationale","deep read-only research with professional verification"},
        {"linguistic_evidence",{{"requested_actions",nlohmann::json::array({"research"})},
            {"negated_actions",nlohmann::json::array()},{"mention_only",nlohmann::json::array()},
            {"scope_constraints",nlohmann::json::array({"read only"})},
            {"ambiguities",nlohmann::json::array()}}},{"clarification",nullptr}};
}
}

int main() {
    using namespace agent_framework;
    using namespace agent_framework::conversation;
    auto parsed=LLMTaskClassifier::parse(valid().dump());
    assert(parsed&&parsed.schema_version==3&&parsed.intent==TaskIntentKind::NewTask);
    assert(parsed.profile==TaskExecutionProfile::CodeChange&&parsed.long_running);
    assert(parsed.effect_class==EffectClass::WorkspaceWrite&&parsed.confidence==0.93);
    assert(parsed.classifier_id=="llm-task-classifier-v3"&&
           parsed.prompt_version=="task-routing-v3"&&!parsed.grants_authority);
    auto semantic=LLMTaskClassifier::parse(valid_v4().dump());
    assert(semantic&&semantic.schema_version==4&&
           semantic.work_shape==WorkShape::LongRunningTask&&
           semantic.effect_class==EffectClass::ReadOnly&&
           semantic.assurance_tier==AssuranceTier::Professional&&
           semantic.profile==TaskExecutionProfile::Professional&&semantic.long_running);
    assert(semantic.classifier_id=="llm-task-classifier-v4"&&
           semantic.prompt_version=="task-routing-v4");
    auto bounded=valid_v4();bounded["work_shape"]="bounded_task";
    bounded["effect_class"]="workspace_write";bounded["assurance_tier"]="functional";
    auto bounded_result=LLMTaskClassifier::parse(bounded.dump());
    assert(bounded_result&&bounded_result.work_shape==WorkShape::BoundedTask&&
           bounded_result.profile==TaskExecutionProfile::CodeChange&&
           !bounded_result.long_running);
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
    assert(!LLMTaskClassifier::parse(uncertain.dump()).requires_confirmation);
    auto ambiguous=valid();ambiguous["linguistic_evidence"]["ambiguities"]={"unclear target"};
    ambiguous["clarification"]={{"question","Which outcome do you want?"},{"options",{
        {{"id","explain"},{"label","Explain only"},{"description","Do not edit files"},
         {"profile","read_only_analysis"}},
        {{"id","implement"},{"label","Implement it"},{"description","Edit and test"},
         {"profile","code_change"}}}}};
    auto clarification=LLMTaskClassifier::parse(ambiguous.dump());
    assert(clarification.requires_confirmation&&clarification.clarification&&
           clarification.clarification->options.size()==2);
    auto semantic_ambiguous=valid_v4();semantic_ambiguous["confidence"]=0.5;
    semantic_ambiguous["linguistic_evidence"]["ambiguities"]={"deliverable unclear"};
    semantic_ambiguous["clarification"]={{"question","Research or implement?"},{"options",{
        {{"id","research"},{"label","Research only"},{"description","Do not edit"},
         {"semantic_patch",{{"work_shape","long_running_task"},{"effect_class","read_only"}}}},
        {{"id","implement"},{"label","Implement"},{"description","Edit workspace"},
         {"semantic_patch",{{"work_shape","bounded_task"},{"effect_class","workspace_write"},
                            {"compatibility_profile","code_change"}}}}}}};
    auto semantic_question=LLMTaskClassifier::parse(semantic_ambiguous.dump());
    assert(semantic_question.requires_confirmation&&semantic_question.clarification&&
           semantic_question.clarification->options.at(1).semantic_patch.at("effect_class")==
               "workspace_write");
    auto bad_evidence=valid();bad_evidence["linguistic_evidence"]["ambiguities"]="none";
    assert(!LLMTaskClassifier::parse(bad_evidence.dump()));
    auto too_large=valid();too_large["rationale"]=std::string(513,'x');
    assert(!LLMTaskClassifier::parse(too_large.dump()));

    // Deterministic fallback is observation-only and can never escalate authority/profile.
    for(const char* input:{"不要发布，先讨论方案","生成分析报告和图片文件",
                           "把登录接口实现并补测试","delete production data"}) {
        auto safe=deterministic_task_classification(input);
        assert(safe&&safe.profile==TaskExecutionProfile::Conversation&&!safe.long_running);
        assert(!safe.requires_confirmation&&!safe.grants_authority&&safe.confidence==0.0);
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
    assert(weak&&weak.profile==TaskExecutionProfile::Conversation&&
           !weak.requires_confirmation);

    TaskSemanticCalibrationMetrics metrics;
    auto over_routed=parsed;over_routed.effect_class=EffectClass::External;
    metrics.observe(over_routed,{EffectClass::ReadOnly,true,false,false,true});
    metrics.observe(semantic,{EffectClass::ReadOnly,true,true,false,false});
    const auto calibration=metrics.snapshot();
    assert(calibration.at("schema")=="agent.task_semantic_calibration/v1");
    assert(calibration.at("samples")==2);
    assert(calibration.at("false_high_effect").at("count")==1);
    assert(calibration.at("missed_planning").at("count")==1);
    assert(calibration.at("unnecessary_clarification").at("count")==0);
    assert(calibration.at("decision_abandonment").at("count")==1);
    assert(!calibration.at("canonical_digest").get<std::string>().empty());
}
