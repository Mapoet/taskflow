#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include <workflow/nodeflow.hpp>

#include "agent/agent_template/compiler.hpp"
#include "agent/agent_template/governance.hpp"
#include "agent/agent_template/planning.hpp"
#include "agent/agent_template/registry.hpp"

namespace agent_framework::agent_template {

struct AgentRunOptions {
    contracts::ContractMetadata metadata;
    AgentHostingMode hosting_mode{AgentHostingMode::Standalone};
    std::string invocation_id;
    std::string session_id;
    std::string model_profiles_digest;
    std::string prompt_revisions_digest;
    std::string capability_snapshot_digest;
    std::string deployment_generation;
    std::string context_projection_ref;
    std::shared_ptr<std::atomic_bool> cancel;
    bool require_completion_authority{true};
};

struct AgentRunResult {
    bool ok{false};
    std::optional<AgentTemplate> agent_template;
    std::optional<SkillCollaborationPlan> plan;
    std::optional<ActiveSkillSession> session;
    std::optional<AgentTemplateInvocation> invocation;
    CompiledExecutionResult execution;
    std::optional<AgentCompletionDecision> completion;
    std::vector<contracts::ContractIssue> issues;
    std::string error_code;
    std::string error_message;
};

class AgentRuntime {
public:
    AgentRuntime(std::shared_ptr<AgentTemplateRegistry> templates,
                 std::shared_ptr<SkillRegistry> skills,
                 std::shared_ptr<SkillRunnerRegistry> runners);

    void set_model_plan_provider(std::shared_ptr<SkillPlanProvider> provider);
    void set_hybrid_plan_provider(std::shared_ptr<SkillPlanProvider> provider);
    void set_completion_authority(std::shared_ptr<AgentCompletionAuthority> authority);

    AgentRunResult run(const TemplateRef& template_ref,
                       const nlohmann::json& input,
                       AgentRunOptions options = {}) const;
    AgentRunResult resume(std::string_view tenant_id,std::string_view invocation_id,
                          std::shared_ptr<std::atomic_bool> cancel = {}) const;

private:
    std::shared_ptr<SkillPlanProvider> provider_for(AgentBusinessMode mode) const;
    std::shared_ptr<AgentTemplateRegistry> templates_;
    std::shared_ptr<SkillRegistry> skills_;
    std::shared_ptr<SkillRunnerRegistry> runners_;
    std::shared_ptr<SkillPlanProvider> model_provider_;
    std::shared_ptr<SkillPlanProvider> hybrid_provider_;
    std::shared_ptr<AgentCompletionAuthority> completion_authority_;
};

class AgentTemplateNode {
public:
    static std::pair<std::shared_ptr<workflow::AnyNode>, tf::Task> create(
        workflow::GraphBuilder& builder,
        const std::string& name,
        const std::vector<std::pair<std::string, std::string>>& input_specs,
        std::shared_ptr<AgentRuntime> runtime,
        TemplateRef template_ref,
        AgentRunOptions options = {},
        const std::string& output_key = "agent_result");
};

class AgentTemplateSubflow {
public:
    static AgentRunResult run(std::shared_ptr<AgentRuntime> runtime,
                              const TemplateRef& template_ref,
                              const workflow::ValueMap& inputs,
                              AgentRunOptions options = {});
};

}  // namespace agent_framework::agent_template
