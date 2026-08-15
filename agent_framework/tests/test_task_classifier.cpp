#include <cassert>

#include "agent/conversation/task_classifier.hpp"

int main() {
    using namespace agent_framework;
    using namespace agent_framework::conversation;
    auto parsed=LLMTaskClassifier::parse(R"(```json
      {"profile":"code_change","long_running":true,"confidence":0.93,
       "rationale":"repository modification","classifier_id":"model-r1"}
    ```)" );
    assert(parsed&&parsed.profile==TaskExecutionProfile::CodeChange&&
           parsed.long_running&&parsed.confidence==0.93);
    assert(!LLMTaskClassifier::parse("not-json"));
    auto artifact=deterministic_task_classification("生成分析报告和图片文件");
    assert(artifact.profile==TaskExecutionProfile::ArtifactDelivery&&artifact.long_running);
    auto configured=apply_task_routing_policy({},TaskExecutionProfile::Professional,
                                               ExecutionTrustProfile::Production);
    assert(configured&&configured.profile==TaskExecutionProfile::Professional&&
           configured.confidence==1.0);
    auto weak=deterministic_task_classification("分析问题");
    weak=apply_task_routing_policy(weak,TaskExecutionProfile::Conversation,
                                   ExecutionTrustProfile::Production);
    assert(!weak&&weak.error=="production_task_classification_confidence_too_low");
}
