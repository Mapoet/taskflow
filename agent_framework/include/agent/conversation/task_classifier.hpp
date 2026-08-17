#pragma once

#include <atomic>
#include <memory>
#include <functional>
#include <optional>
#include <string>

#include "agent/conversation/task_intent.hpp"
#include "agent/conversation/task_semantics.hpp"
#include "agent/conversation/types.hpp"
#include "agent/graph_executor/graph_executor.hpp"
#include "agent/llm_client/llm_client.hpp"

namespace agent_framework::conversation {

struct TaskClarificationOption {
    std::string id;
    std::string label;
    std::string description;
    TaskExecutionProfile profile{TaskExecutionProfile::Conversation};
    nlohmann::json semantic_patch=nlohmann::json::object();
};

struct TaskClarificationProposal {
    std::string question;
    std::vector<TaskClarificationOption> options;
};

struct TaskClassification {
    int schema_version{4};
    TaskIntentKind intent{TaskIntentKind::NewTask};
    WorkShape work_shape{WorkShape::SingleTurn};
    AssuranceTier assurance_tier{AssuranceTier::Basic};
    TaskExecutionProfile profile{TaskExecutionProfile::Conversation};
    EffectClass effect_class{EffectClass::None};
    bool long_running{false};
    double confidence{0.0};
    std::string rationale;
    std::string classifier_id;
    std::string prompt_version;
    std::string decision_id;
    LinguisticEvidence linguistic_evidence;
    std::optional<TaskClarificationProposal> clarification;
    bool requires_confirmation{false};
    bool grants_authority{false};
    std::string error;
    explicit operator bool() const noexcept { return error.empty(); }
};

struct TaskClassifierInvocation {
    std::string invocation_id,provider,model,classifier_id,prompt_revision;
    std::string deployment_revision{"runtime-configured"},memory_view_digest;
    std::string input_digest,output_digest,outcome,error_code;
    std::uint64_t latency_ms{0};
    std::optional<std::uint64_t> input_tokens,output_tokens,cached_input_tokens;
    std::optional<double> cost_usd;
    int schema_version{4};
    double confidence{0.0};
    bool fallback_used{false};
};

struct TaskSemanticCalibrationSample {
    EffectClass expected_effect{EffectClass::None};
    bool expected_planning{false};
    bool actual_planning{false};
    bool expected_clarification{false};
    bool decision_abandoned{false};
};

// Thread-safe aggregate for telemetry/API export. No prompt or user text is retained.
class TaskSemanticCalibrationMetrics {
public:
    void observe(const TaskClassification&,const TaskSemanticCalibrationSample&);
    nlohmann::json snapshot() const;
private:
    std::atomic<std::uint64_t> samples_{0},false_high_effect_{0},missed_planning_{0},
        unnecessary_planning_{0},unnecessary_clarification_{0},decision_abandonment_{0};
};
using TaskClassifierObserver=std::function<void(const TaskClassifierInvocation&)>;

class TaskClassifier {
public:
    virtual ~TaskClassifier() = default;
    virtual TaskClassification classify(std::string_view input,
                                         bool has_active_task) = 0;
    virtual TaskClassification classify_observed(std::string_view input,
        bool has_active_task,TaskClassifierObserver observer) {
        auto result=classify(input,has_active_task);
        (void)observer;
        return result;
    }
};

class LLMTaskClassifier final : public TaskClassifier {
public:
    LLMTaskClassifier(std::shared_ptr<LLMClient> client, std::string provider = {},
                      TaskClassifierObserver observer = {});
    TaskClassification classify(std::string_view input,
                                bool has_active_task) override;
    TaskClassification classify_observed(std::string_view input,
        bool has_active_task,TaskClassifierObserver observer) override;
    static TaskClassification parse(std::string_view response);
private:
    std::shared_ptr<LLMClient> client_;
    std::string provider_;
    TaskClassifierObserver observer_;
};

TaskClassification deterministic_task_classification(std::string_view input);
TaskClassification apply_task_routing_policy(TaskClassification model,
                                              std::optional<TaskExecutionProfile> configured,
                                              ExecutionTrustProfile trust);

}  // namespace agent_framework::conversation
