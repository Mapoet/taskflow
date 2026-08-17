#pragma once

#include <memory>
#include <optional>
#include <string>

#include "agent/conversation/task_intent.hpp"
#include "agent/conversation/types.hpp"
#include "agent/graph_executor/graph_executor.hpp"
#include "agent/llm_client/llm_client.hpp"

namespace agent_framework::conversation {

struct TaskClassification {
    int schema_version{2};
    TaskIntentKind intent{TaskIntentKind::NewTask};
    TaskExecutionProfile profile{TaskExecutionProfile::Conversation};
    EffectClass effect_class{EffectClass::None};
    bool long_running{false};
    double confidence{0.0};
    std::string rationale;
    std::string classifier_id;
    std::string prompt_version;
    std::string decision_id;
    LinguisticEvidence linguistic_evidence;
    bool requires_confirmation{false};
    bool grants_authority{false};
    std::string error;
    explicit operator bool() const noexcept { return error.empty(); }
};

class TaskClassifier {
public:
    virtual ~TaskClassifier() = default;
    virtual TaskClassification classify(std::string_view input,
                                         bool has_active_task) = 0;
};

class LLMTaskClassifier final : public TaskClassifier {
public:
    LLMTaskClassifier(std::shared_ptr<LLMClient> client, std::string provider = {});
    TaskClassification classify(std::string_view input,
                                bool has_active_task) override;
    static TaskClassification parse(std::string_view response);
private:
    std::shared_ptr<LLMClient> client_;
    std::string provider_;
};

TaskClassification deterministic_task_classification(std::string_view input);
TaskClassification apply_task_routing_policy(TaskClassification model,
                                              std::optional<TaskExecutionProfile> configured,
                                              ExecutionTrustProfile trust);

}  // namespace agent_framework::conversation
