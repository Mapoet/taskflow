#pragma once

#include <memory>
#include <string>

#include "agent/conversation/types.hpp"
#include "agent/graph_executor/graph_executor.hpp"
#include "agent/llm_client/llm_client.hpp"

namespace agent_framework::conversation {

struct TaskClassification {
    TaskExecutionProfile profile{TaskExecutionProfile::Conversation};
    bool long_running{false};
    double confidence{0.0};
    std::string rationale;
    std::string classifier_id;
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
                                              TaskExecutionProfile configured,
                                              ExecutionTrustProfile trust);

}  // namespace agent_framework::conversation
