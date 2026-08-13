#include <agent/agent_template/operations.hpp>

#include <cassert>
#include <iostream>

using namespace agent_framework;
using namespace agent_framework::agent_template;

int main() {
    AgentRunResult result;
    result.ok = true;
    result.agent_template.emplace();
    result.agent_template->template_id = "research-agent";
    result.agent_template->revision = 4;
    result.plan.emplace();
    result.plan->plan_id = "research-plan";
    result.plan->revision = 7;
    SkillPlanNode node;
    node.node_id = "verify";
    node.resolved_skill_id = "citation-verifier";
    node.resolved_skill_version = "2.0.0";
    node.runner = SkillRunnerKind::ChildAgent;
    node.role = SkillRole::Verifier;
    result.plan->nodes = {node};
    result.session.emplace();
    result.session->session_id = "session-1";
    result.session->registry_generation = "42";
    result.session->deployment_generation = "prod-9";
    result.invocation.emplace();
    result.invocation->invocation_id = "invocation-1";
    result.invocation->template_ref = {"research-agent", 4, "sha256:template"};
    result.invocation->plan_digest = "sha256:plan";
    result.invocation->skill_session_digest = "sha256:session";
    result.invocation->business_mode = AgentBusinessMode::Hybrid;
    result.invocation->hosting_mode = AgentHostingMode::Conversation;
    SkillRunnerReceipt receipt;
    receipt.node_id = "verify";
    receipt.terminal_state = RunnerLifecycleState::Succeeded;
    receipt.output_digest = "sha256:output";
    receipt.evidence.push_back({"evidence-1", "verification", "sha256:evidence", ""});
    result.execution.receipts = {receipt};
    result.completion = AgentCompletionDecision{true, "task_closure_controller",
        "all_mandatory_criteria_verified", "sha256:decision", {"evidence-1"}, {}};
    Phase4OperationsSnapshot snapshot;
    snapshot.snapshot_id = "snapshot";
    snapshot.tenant_id = "tenant";
    snapshot.run_id = "run";
    snapshot.task_id = "task";
    snapshot.updated_at = "2026-08-13T00:00:00Z";
    AgentTemplateOperationsProjection::merge(snapshot, result);
    assert(snapshot.agent_templates.size() == 1);
    assert(snapshot.skill_nodes.size() == 1);
    assert(snapshot.task_completion_verified);
    auto document = Phase4OperationsProjection::to_json(snapshot);
    auto decoded = Phase4OperationsProjection::from_json(document);
    assert(decoded.agent_templates.front().hosting_mode == "conversation");
    assert(decoded.skill_nodes.front().state == "succeeded");
    const auto text = Phase4OperationsProjection::render_text(decoded);
    assert(text.find("research-agent@r4") != std::string::npos);
    assert(text.find("citation-verifier@2.0.0") != std::string::npos);
    std::cout << "test_agent_template_operations: ok\n";
}
