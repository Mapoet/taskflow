#pragma once

#include <deque>
#include <future>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "agent/llm_client/llm_client.hpp"
#include "agent/llm_runtime/runtime.hpp"

namespace phase4_llm_test {

using namespace agent_framework;
using namespace agent_framework::llm_runtime;
using json = nlohmann::json;

inline contracts::ContractMetadata metadata(std::string tenant = "tenant-a") {
    contracts::ContractMetadata value;
    value.identity.tenant_id = std::move(tenant);
    value.identity.organization_id = "org-a";
    value.identity.principal_id = "user-a";
    value.identity.project_id = "project-a";
    value.identity.task_id = "task-a";
    value.identity.run_id = "run-a";
    value.identity.plan_id = "plan-a";
    return value;
}

inline LLMRoleProfile profile() {
    LLMRoleProfile value;
    value.metadata = metadata();
    value.profile_id = "planning.architect";
    value.revision = "profile-r1";
    value.role = "planning.architect";
    value.provider_pool = {"primary", "fallback"};
    value.reasoning_effort = ReasoningEffort::High;
    value.temperature = 0.1;
    value.max_context_tokens = 8192;
    value.max_output_tokens = 256;
    value.prompt_id = "planning.prompt";
    value.prompt_revision = "prompt-r1";
    value.memory_view_profile = "planning";
    value.required_capabilities = {"repo_read"};
    value.allowed_regions = {"local"};
    value.independence_group = "planner";
    value.evidence_authority = EvidenceAuthority::Advisory;
    value.timeout_ms = 5000;
    value.max_attempts = 3;
    value.max_fallbacks = 1;
    value.max_cost_usd = 0.10;
    value.calibration_revision = "cal-r1";
    value.provider_parameters = {{"seed", 7}};
    return value;
}

inline PromptRevision prompt() {
    PromptRevision value;
    value.metadata = metadata();
    value.prompt_id = "planning.prompt";
    value.revision = "prompt-r1";
    value.system_template = "Architect for {{topic}}";
    value.user_template = "Analyze {{topic}}";
    value.input_schema = {{"type", "object"},
        {"properties", {{"topic", {{"type", "string"}}}}},
        {"required", {"topic"}}, {"additionalProperties", false}};
    value.output_schema = {{"type", "object"},
        {"properties", {{"ok", {{"type", "boolean"}}}}},
        {"required", {"ok"}}, {"additionalProperties", false}};
    value.structured_output_required = true;
    value.max_repair_attempts = 1;
    value.compatibility_class = "planning-v1";
    return value;
}

inline RoleCalibrationRecord calibration(
    std::string provider = {}, std::string model = {}) {
    RoleCalibrationRecord value;
    value.metadata = metadata();
    value.calibration_id = "cal-r1";
    value.role = "planning.architect";
    value.profile_id = "planning.architect";
    value.profile_revision = "profile-r1";
    value.prompt_revision = "prompt-r1";
    value.provider = std::move(provider);
    value.model = std::move(model);
    value.dataset_revision = "dataset-r1";
    value.metrics = {{"schema_success", 1.0}};
    value.thresholds = {{"schema_success", 0.99}};
    value.approved = true;
    value.decision_id = "decision-r1";
    return value;
}

inline ProviderModel candidate(std::string id, std::string provider_name,
                               std::string model, int priority) {
    ProviderModel value;
    value.candidate_id = std::move(id);
    value.provider = std::move(provider_name);
    value.model = std::move(model);
    value.adapter_revision = "adapter-r1";
    value.capabilities = {"repo_read"};
    value.regions = {"local"};
    value.model_family = "test";
    value.independence_group = "model-" + value.candidate_id;
    value.max_context_tokens = 32768;
    value.input_cost_per_million = 1.0;
    value.output_cost_per_million = 2.0;
    value.priority = priority;
    return value;
}

inline RoleInvocationRequest request(std::string id = "invocation-a") {
    RoleInvocationRequest value;
    value.metadata = metadata();
    value.invocation_id = std::move(id);
    value.trace_id = "trace-a";
    value.parent_span_id = "parent-a";
    value.profile_id = "planning.architect";
    value.profile_revision = "profile-r1";
    value.prompt_variables = {{"topic", "durable workflows"}};
    value.memory_view = {"snapshot-a", "planning", "sha256:view-a"};
    value.granted_capabilities = {"repo_read"};
    value.required_region = "local";
    value.estimated_input_tokens = 512;
    value.policy_revision = "policy-r1";
    return value;
}

class ScriptedAdapter final : public ModelAdapter {
public:
    using Step = std::function<LLMOutput()>;

    void push(Step step) { steps_.push_back(std::move(step)); }
    std::size_t calls() const noexcept { return calls_; }
    const std::vector<RenderedPrompt>& rendered() const noexcept { return rendered_; }

    std::future<LLMOutput> invoke(
        const LLMInput& input,
        std::function<void(std::string_view)> callback = nullptr) override {
        RenderedPrompt rendered;
        rendered.messages = {{{"role", "system"}, {"content", input.system_prompt}},
                             {{"role", "user"}, {"content", input.user_prompt}}};
        rendered.model_config = input.model_config;
        return invoke_with_rendered(rendered, std::move(callback));
    }

    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& rendered,
        std::function<void(std::string_view)> callback = nullptr) override {
        return invoke_with_rendered_channels(rendered, std::move(callback), nullptr);
    }

    std::future<LLMOutput> invoke_with_rendered_channels(
        const RenderedPrompt& rendered,
        std::function<void(std::string_view)> answer_callback,
        std::function<void(std::string_view)> thinking_callback) override {
        rendered_.push_back(rendered);
        ++calls_;
        if(steps_.empty()) throw std::runtime_error("script exhausted");
        auto step = std::move(steps_.front());
        steps_.pop_front();
        return std::async(std::launch::deferred,
            [step = std::move(step), answer_callback = std::move(answer_callback),
             thinking_callback = std::move(thinking_callback)]() mutable {
                auto output = step();
                if(thinking_callback && !output.reasoning.empty())
                    thinking_callback("provider-visible-summary");
                if(answer_callback && !output.final_answer.empty())
                    answer_callback(output.final_answer);
                return output;
            });
    }

    std::vector<ToolMeta> get_available_tools() const override { return {}; }
    void configure(const ModelConfig& config) override { config_ = config; }
    std::string get_model_name() const override { return config_.model_name; }
    bool supports_multimodal() const override { return false; }

private:
    ModelConfig config_;
    std::deque<Step> steps_;
    std::vector<RenderedPrompt> rendered_;
    std::size_t calls_{0};
};

inline LLMOutput output(std::string answer, std::uint64_t input_tokens = 10,
                        std::uint64_t output_tokens = 4) {
    LLMOutput value;
    value.final_answer = std::move(answer);
    value.reasoning = "private-chain-of-thought-do-not-persist";
    value.is_final = true;
    value.usage = LLMUsage{input_tokens, output_tokens, std::nullopt, 0.001,
                           "provider:test", {}};
    return value;
}

inline void publish_baseline(const std::shared_ptr<LLMRuntimeStore>& store) {
    if(!store->publish_profile(profile()).ok() || !store->publish_prompt(prompt()).ok() ||
       !store->publish_calibration(calibration()).ok())
        throw std::runtime_error("failed to publish baseline LLM runtime documents");
}

}  // namespace phase4_llm_test
