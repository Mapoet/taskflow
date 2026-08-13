#include <agent/agent_template/runtime.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace agent_framework;
using namespace agent_framework::agent_template;
namespace fs = std::filesystem;

namespace
{
    contracts::ContractMetadata metadata()
    {
        contracts::ContractMetadata value;
        value.identity.tenant_id = "tenant-runtime";
        value.identity.task_id = "task-runtime";
        return value;
    }
    void write_skill(const fs::path &root)
    {
        fs::create_directories(root / "echo");
        std::ofstream out(root / "echo/SKILL.md");
        out << R"(---
api-version: agent.taskflow/v1
kind: Skill
name: echo-skill
version: 1.0.0
description: deterministic echo capability
tags: [echo]
trigger-keywords: [echo]
permissions:
  tools: [echo]
---
Echo the input.)";
    }
    AgentTemplate make_template(const SkillRegistrySnapshot &snapshot)
    {
        AgentTemplate value;
        value.metadata = metadata();
        value.template_id = "echo-agent";
        value.name = "Echo Agent";
        value.business_mode = AgentBusinessMode::DirectiveDriven;
        value.permissions.tools = {"echo"};
        value.budgets.max_parallelism = 2;
        AgentRoleDefinition role;
        role.role_id = "echo";
        role.selector.skill_id = "echo-skill";
        role.selector.candidates = {"echo-skill"};
        value.roles = {role};
        SkillCollaborationPlan plan;
        plan.metadata = metadata();
        plan.plan_id = "echo-plan";
        plan.budget = value.budgets;
        plan.output_assembly = {{"from", "echo"}};
        SkillPlanNode node;
        node.node_id = "echo";
        node.selector = role.selector;
        node.runner = SkillRunnerKind::LocalCapability;
        node.requested_permissions.tools = {"echo"};
        node.output.evidence_required = false;
        value.workflow_skeleton = plan;
        value.workflow_skeleton->nodes = {node};
        (void)snapshot;
        return value;
    }
} // namespace

int main()
{
    const auto root = fs::temp_directory_path() / "agent-template-runtime";
    std::error_code ec;
    fs::remove_all(root, ec);
    write_skill(root / "skills");
    auto skills = std::make_shared<SkillRegistry>(root / "skills");
    skills->scan_or_reload();
    assert(skills->valid());
    auto registry = std::make_shared<SQLiteAgentTemplateRegistry>((root / "registry.sqlite3").string());
    const auto agent_template = make_template(skills->snapshot());
    assert(registry->publish(agent_template).ok());
    const auto template_digest = encode(agent_template).at("canonical_digest").get<std::string>();
    auto runners = std::make_shared<SkillRunnerRegistry>();
    assert(runners->register_runner(std::make_shared<CallbackSkillRunner>(
        SkillRunnerKind::LocalCapability, [](const RunnerRequest &request)
        { return nlohmann::json{{"echo", request.input.value("message", "")}}; })));
    auto runtime = std::make_shared<AgentRuntime>(registry, skills, runners);
    runtime->set_completion_authority(std::make_shared<CallbackCompletionAuthority>(
        [](const CompletionEvidence& evidence) {
            AgentCompletionDecision decision;
            decision.accepted = !evidence.receipts.empty();
            decision.authority = "test-assurance-closure";
            decision.reason_code = decision.accepted ? "verified" : "evidence_missing";
            decision.decision_digest = "sha256:closure";
            return decision;
        }));
    AgentRunOptions options;
    options.metadata = metadata();
    options.model_profiles_digest = "sha256:model";
    options.prompt_revisions_digest = "sha256:prompt";
    options.deployment_generation = "deployment-7";
    auto direct = runtime->run({"echo-agent", 1, template_digest},
                               {{"message", "hello"}}, options);
    assert(direct.ok);
    assert(direct.execution.output.at("echo") == "hello");
    assert(direct.completion && direct.completion->authority == "test-assurance-closure");
    assert(direct.session->skills.size() == 1);
    assert(direct.session->skills.front().version == "1.0.0");
    assert(registry->load_invocation("tenant-runtime", direct.invocation->invocation_id));

    workflow::GraphBuilder graph("agent-node");
    graph.create_any_source("source", {{"message", std::string("workflow")}});
    AgentRunOptions node_options = options;
    node_options.invocation_id = "workflow-invocation";
    node_options.session_id = "workflow-session";
    AgentTemplateNode::create(graph, "agent", {{"source", "message"}}, runtime,
                              {"echo-agent", 1, template_digest}, node_options);
    tf::Executor executor(2);
    graph.run(executor);
    auto node_result = std::any_cast<AgentRunResult>(graph.get_latest_output("agent", "agent_result"));
    assert(node_result.ok);
    assert(node_result.invocation->hosting_mode == AgentHostingMode::WorkflowNode);
    assert(node_result.execution.output.at("echo") == "workflow");

    AgentRunOptions subflow_options = options;
    subflow_options.invocation_id = "subflow-invocation";
    subflow_options.session_id = "subflow-session";
    auto subflow = AgentTemplateSubflow::run(runtime, {"echo-agent", 1, template_digest},
                                             {{"message", std::string("subflow")}},
                                             subflow_options);
    assert(subflow.ok);
    assert(subflow.invocation->hosting_mode == AgentHostingMode::WorkflowSubflow);

    auto mismatch = runtime->run({"echo-agent", 1, "sha256:wrong"},
                                 nlohmann::json::object(), options);
    assert(!mismatch.ok && mismatch.error_code == "template_digest_mismatch");
    fs::remove_all(root, ec);
    std::cout << "test_agent_template_runtime: ok\n";
}
