#include <agent/agent_template/types.hpp>

#include <cassert>
#include <iostream>

using namespace agent_framework;
using namespace agent_framework::agent_template;

namespace {
contracts::ContractMetadata metadata(const char* task) {
    contracts::ContractMetadata m;
    m.identity.tenant_id = "tenant-a";
    m.identity.task_id = task;
    return m;
}
SkillPlanNode node() {
    SkillPlanNode n;
    n.node_id = "retrieval";
    n.role = SkillRole::Worker;
    n.selector.skill_id = "literature-search";
    n.resolved_skill_id = "literature-search";
    n.resolved_skill_version = "1.2.0";
    n.resolved_skill_digest = "sha256:skill";
    n.runner = SkillRunnerKind::LocalCapability;
    n.output.schema = {{"type", "object"}};
    n.output.evidence_required = true;
    n.requested_permissions.tools = {"web_search"};
    return n;
}
}

int main() {
    SkillCollaborationPlan plan;
    plan.metadata = metadata("task-at0");
    plan.plan_id = "plan-1";
    plan.nodes = {node()};
    plan.output_assembly = {{"from", "retrieval"}};
    const auto plan_json = encode(plan);
    std::vector<contracts::ContractIssue> issues;
    const auto decoded_plan = decode_collaboration_plan(plan_json, {}, &issues);
    assert(decoded_plan && issues.empty());
    assert(decoded_plan->nodes.at(0).resolved_skill_id == "literature-search");
    assert(encode(*decoded_plan) == plan_json);

    AgentTemplate templ;
    templ.metadata = metadata("task-at0");
    templ.template_id = "research-agent";
    templ.name = "Research Agent";
    templ.business_mode = AgentBusinessMode::Hybrid;
    templ.permissions.tools = {"web_search"};
    templ.roles.push_back({"retriever", SkillRole::Worker,
                           SkillSelector{std::string("literature-search")}, 1, 2, true});
    templ.workflow_skeleton = plan;
    templ.completion_contract = {{"verifier_required", true}};
    const auto template_json = encode(templ);
    const auto decoded_template = decode_agent_template(template_json, {}, &issues);
    assert(decoded_template && decoded_template->workflow_skeleton);
    assert(encode(*decoded_template) == template_json);

    ActiveSkillSession session;
    session.metadata = metadata("task-at0");
    session.session_id = "session-1";
    session.plan_digest = plan_json.at("canonical_digest");
    session.registry_generation = "registry-7";
    session.skills.push_back({"literature-search", "1.2.0", "sha256:skill", {},
                              templ.permissions, "sha256:caps"});
    session.effective_permissions = templ.permissions;
    session.capability_snapshot_digest = "sha256:caps";
    const auto session_json = encode(session);
    assert(decode_active_skill_session(session_json));

    // Business and hosting modes are independent dimensions.
    for(const auto business : {AgentBusinessMode::ModelDriven,
                               AgentBusinessMode::DirectiveDriven,
                               AgentBusinessMode::Hybrid,
                               AgentBusinessMode::FixedWorkflow}) {
        for(const auto hosting : {AgentHostingMode::Standalone,
                                  AgentHostingMode::Conversation,
                                  AgentHostingMode::WorkflowNode,
                                  AgentHostingMode::WorkflowSubflow,
                                  AgentHostingMode::RemoteA2A}) {
            AgentTemplateInvocation invocation;
            invocation.metadata = metadata("task-at0");
            invocation.invocation_id = "inv-" + to_string(business) + "-" + to_string(hosting);
            invocation.template_ref = {templ.template_id, templ.revision,
                                       template_json.at("canonical_digest")};
            invocation.plan_digest = plan_json.at("canonical_digest");
            invocation.skill_session_digest = session_json.at("canonical_digest");
            invocation.permissions = templ.permissions;
            invocation.business_mode = business;
            invocation.hosting_mode = hosting;
            auto document = encode(invocation);
            auto decoded = decode_template_invocation(document);
            assert(decoded && decoded->business_mode == business &&
                   decoded->hosting_mode == hosting);
        }
    }

    auto future = template_json;
    future["schema_version"] = 99;
    future.erase("canonical_digest");
    future["canonical_digest"] = *contracts::embedded_digest(future);
    issues.clear();
    assert(!decode_agent_template(future, {}, &issues));
    assert(!issues.empty() && issues.front().code == "future_schema_version");

    auto tampered = plan_json;
    tampered["payload"]["plan_id"] = "tampered";
    issues.clear();
    assert(!decode_collaboration_plan(tampered, {}, &issues));
    assert(!issues.empty());

    SkillRunnerReceipt receipt;
    receipt.invocation_id = "inv-1";
    receipt.node_id = "retrieval";
    receipt.runner = SkillRunnerKind::Mcp;
    receipt.terminal_state = RunnerLifecycleState::Succeeded;
    receipt.artifacts.push_back({"artifact-1", "application/json", "sha256:a", "cas://a"});
    assert(encode(receipt).at("artifacts").size() == 1);
    std::cout << "test_agent_template_contracts: ok\n";
}
