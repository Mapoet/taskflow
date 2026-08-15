#include "agent/conversation/task_classifier.hpp"
#include "agent/graph_executor/graph_executor.hpp"

#include <algorithm>
#include <cctype>

namespace agent_framework::conversation {
namespace {
std::string lower(std::string_view input) {
    std::string out(input);
    std::transform(out.begin(),out.end(),out.begin(),[](unsigned char c){return char(std::tolower(c));});
    return out;
}
std::string json_body(std::string_view response) {
    const auto begin=response.find('{'),end=response.rfind('}');
    return begin==std::string_view::npos||end==std::string_view::npos||end<begin
        ? std::string{}:std::string(response.substr(begin,end-begin+1));
}
}

LLMTaskClassifier::LLMTaskClassifier(std::shared_ptr<LLMClient> client,std::string provider)
    :client_(std::move(client)),provider_(std::move(provider)) {
    if(!client_) throw std::invalid_argument("LLM task classifier requires client");
}

TaskClassification LLMTaskClassifier::parse(std::string_view response) {
    try {
        const auto body=json_body(response);if(body.empty())return {{},false,0,"","","classifier_output_not_json"};
        const auto value=nlohmann::json::parse(body);
        auto profile=task_execution_profile(value.value("profile",""));
        if(!profile)return {{},false,0,"","","classifier_profile_invalid"};
        TaskClassification out;out.profile=*profile;out.long_running=value.value("long_running",false);
        out.confidence=value.value("confidence",0.0);out.rationale=value.value("rationale","");
        out.classifier_id=value.value("classifier_id","llm-task-classifier-v1");
        if(out.confidence<0.0||out.confidence>1.0)out.error="classifier_confidence_invalid";
        return out;
    } catch(const std::exception& e) { TaskClassification out;out.error=e.what();return out; }
}

TaskClassification LLMTaskClassifier::classify(std::string_view input,bool has_active_task) {
    LLMInput request;
    request.system_prompt="You are a task routing controller. Return JSON only with keys: "
        "profile (conversation|read_only_analysis|artifact_delivery|code_change|external_action|professional), "
        "long_running (boolean), confidence (0..1), rationale (short), classifier_id. "
        "Classify by required work, side effects, artifacts and verification depth; do not answer the task.";
    request.user_prompt=nlohmann::json({{"input",input},{"has_active_task",has_active_task}}).dump();
    ModelConfig config;config.temperature=0.0;config.stream=false;config.max_tokens=300;
    request.model_config=config;
    try {
        const auto output=client_->invoke(request,provider_).get();
        return parse(output.final_answer.empty()?output.reasoning:output.final_answer);
    } catch(const std::exception& e) { TaskClassification out;out.error=e.what();return out; }
}

TaskClassification deterministic_task_classification(std::string_view input) {
    TaskClassification out;out.classifier_id="deterministic-task-policy-v1";out.confidence=0.55;
    const auto value=lower(input);
    auto contains=[&](std::string_view token){return value.find(token)!=std::string::npos;};
    if(contains("修改")||contains("实现")||contains("代码")||contains("build")||contains("test"))
        out.profile=TaskExecutionProfile::CodeChange;
    else if(contains("生成")||contains("报告")||contains("图")||contains("文件"))
        out.profile=TaskExecutionProfile::ArtifactDelivery;
    else if(contains("部署")||contains("发送")||contains("删除")||contains("发布"))
        out.profile=TaskExecutionProfile::ExternalAction;
    else if(contains("验证")||contains("验收")||contains("专业")||contains("全面分析"))
        out.profile=TaskExecutionProfile::Professional;
    else if(contains("分析")||contains("研究"))out.profile=TaskExecutionProfile::ReadOnlyAnalysis;
    out.long_running=out.profile==TaskExecutionProfile::CodeChange||
        out.profile==TaskExecutionProfile::ArtifactDelivery||
        out.profile==TaskExecutionProfile::ExternalAction||out.profile==TaskExecutionProfile::Professional;
    out.rationale="deterministic safety fallback";return out;
}

TaskClassification apply_task_routing_policy(TaskClassification model,
    TaskExecutionProfile configured,ExecutionTrustProfile trust) {
    if(configured!=TaskExecutionProfile::Conversation) {
        model.error.clear();model.profile=configured;
        model.long_running=configured!=TaskExecutionProfile::ReadOnlyAnalysis;
        model.confidence=1.0;model.rationale="deployment configured task profile";
    }
    if(!model)return model;
    if(trust==ExecutionTrustProfile::Production&&model.confidence<0.70) {
        model.error="production_task_classification_confidence_too_low";
    }
    return model;
}

}  // namespace agent_framework::conversation
