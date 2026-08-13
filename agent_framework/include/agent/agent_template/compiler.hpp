#pragma once
#include <map>
#include <memory>
#include "agent/agent_template/runner.hpp"
namespace agent_framework::agent_template
{
    struct CompiledExecutionResult
    {
        bool ok{false};
        bool suspended{false};
        std::string checkpoint_ref;
        nlohmann::json resume_snapshot = nlohmann::json::object();
        std::map<std::string, nlohmann::json> node_outputs;
        std::vector<SkillRunnerReceipt> receipts;
        std::vector<RunnerEvent> events;
        nlohmann::json output = nlohmann::json::object();
        std::string error_code;
        std::string error_message;
    };
    class SkillWorkflowCompiler
    {
    public:
        explicit SkillWorkflowCompiler(std::shared_ptr<SkillRunnerRegistry> runners) : runners_(std::move(runners)) {}
        CompiledExecutionResult execute(const SkillCollaborationPlan &, const AgentTemplateInvocation &, const ActiveSkillSession &, const nlohmann::json &, std::shared_ptr<std::atomic_bool> cancel = {}, const nlohmann::json &resume_snapshot = {}) const;

    private:
        std::shared_ptr<SkillRunnerRegistry> runners_;
    };
}
