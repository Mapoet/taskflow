#include "agent/agent_template/operations.hpp"

#include <algorithm>
#include <map>

namespace agent_framework::agent_template {
namespace {
template <typename Ref>
std::vector<std::string> ids(const std::vector<Ref>& refs) {
    std::vector<std::string> out;
    for (const auto& ref : refs) out.push_back(ref.id.empty() ? ref.uri : ref.id);
    return out;
}
}  // namespace

void AgentTemplateOperationsProjection::merge(Phase4OperationsSnapshot& snapshot,
                                               const AgentRunResult& result) {
    if (!result.agent_template || !result.plan || !result.session || !result.invocation) return;
    OperationsAgentTemplate agent;
    agent.template_id = result.agent_template->template_id;
    agent.template_revision = result.agent_template->revision;
    agent.template_digest = result.invocation->template_ref.digest;
    agent.invocation_id = result.invocation->invocation_id;
    agent.business_mode = to_string(result.invocation->business_mode);
    agent.hosting_mode = to_string(result.invocation->hosting_mode);
    agent.plan_id = result.plan->plan_id;
    agent.plan_revision = result.plan->revision;
    agent.plan_digest = result.invocation->plan_digest;
    agent.session_id = result.session->session_id;
    agent.session_digest = result.invocation->skill_session_digest;
    agent.registry_generation = result.session->registry_generation;
    agent.deployment_generation = result.session->deployment_generation;
    if (result.completion) {
        agent.completion_authority = result.completion->authority;
        agent.completion_reason = result.completion->reason_code;
    }
    snapshot.agent_templates.push_back(std::move(agent));
    snapshot.plan_revision = std::max(snapshot.plan_revision, result.plan->revision);
    snapshot.completion_authority = result.completion ? result.completion->authority : "none";
    snapshot.task_completion_verified = result.ok && result.completion && result.completion->accepted;
    snapshot.task_closure_reason = result.completion ? result.completion->reason_code : result.error_code;
    snapshot.task_closure_state = snapshot.task_completion_verified ? "completed_verified" : "running";
    std::map<std::string, SkillRunnerReceipt> receipts;
    for (const auto& receipt : result.execution.receipts) receipts[receipt.node_id] = receipt;
    for (const auto& node : result.plan->nodes) {
        OperationsSkillNode projected;
        projected.node_id = node.node_id;
        projected.skill_id = node.resolved_skill_id.value_or("");
        projected.skill_version = node.resolved_skill_version;
        projected.runner = to_string(node.runner);
        projected.role = to_string(node.role);
        const auto found = receipts.find(node.node_id);
        projected.state = found == receipts.end() ? "pending" : to_string(found->second.terminal_state);
        if (found != receipts.end()) {
            projected.output_digest = found->second.output_digest;
            projected.evidence_refs = ids(found->second.evidence);
            projected.artifact_refs = ids(found->second.artifacts);
        }
        snapshot.skill_nodes.push_back(std::move(projected));
    }
}

}  // namespace agent_framework::agent_template
