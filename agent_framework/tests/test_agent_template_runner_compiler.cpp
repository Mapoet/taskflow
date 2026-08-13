#include <agent/agent_template/compiler.hpp>
#include <cassert>
#include <iostream>
using namespace agent_framework;
using namespace agent_framework::agent_template;
int main()
{
    auto registry = std::make_shared<SkillRunnerRegistry>();
    for (auto kind : {SkillRunnerKind::InlinePrompt, SkillRunnerKind::LocalCapability, SkillRunnerKind::SandboxedProcess, SkillRunnerKind::Cli, SkillRunnerKind::Mcp, SkillRunnerKind::ChildAgent, SkillRunnerKind::NestedWorkflow, SkillRunnerKind::HumanApproval})
        assert(registry->register_runner(std::make_shared<CallbackSkillRunner>(kind, [](const RunnerRequest &r)
                                                                               {auto out=r.input;out["executed_by"]=to_string(r.node.runner);if(out.contains("value"))out["value"]=out["value"].get<int>()+1;return out; })));
    SkillCollaborationPlan p;
    p.plan_id = "pipeline";
    p.budget.max_parallelism = 2;
    SkillPlanNode a;
    a.node_id = "a";
    a.resolved_skill_id = "skill-a";
    a.runner = SkillRunnerKind::LocalCapability;
    SkillPlanNode b = a;
    b.node_id = "b";
    b.runner = SkillRunnerKind::Mcp;
    b.input_mapping = {{"value", {{"from", "a"}, {"path", "/value"}}}};
    SkillPlanNode c = a;
    c.node_id = "c";
    c.runner = SkillRunnerKind::ChildAgent;
    c.input_mapping = b.input_mapping;
    p.nodes = {a, b, c};
    p.edges = {{"a", "b", {}}, {"a", "c", {}}};
    p.output_assembly = {{"from", "b"}};
    AgentTemplateInvocation invocation;
    invocation.invocation_id = "inv";
    ActiveSkillSession session;
    SkillWorkflowCompiler compiler(registry);
    auto result = compiler.execute(p, invocation, session, {{"value", 1}});
    assert(result.ok);
    assert(result.node_outputs.at("a").at("value") == 2);
    assert(result.node_outputs.at("b").at("value") == 3);
    assert(result.node_outputs.at("c").at("value") == 3);
    assert(result.receipts.size() == 3);
    assert(result.events.size() == 12);
    assert(result.output.at("executed_by") == "mcp");
    auto missing = p;
    missing.nodes[1].runner = SkillRunnerKind::HumanApproval;
    auto limited = std::make_shared<SkillRunnerRegistry>();
    limited->register_runner(registry->resolve(SkillRunnerKind::LocalCapability));
    assert(!SkillWorkflowCompiler(limited).execute(missing, invocation, session, {{"value", 1}}).ok);
    std::cout << "test_agent_template_runner_compiler: ok\n";
}
