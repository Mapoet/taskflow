#include <agent/agent_template/planning.hpp>
#include <cassert>
#include <iostream>
using namespace agent_framework;
using namespace agent_framework::agent_template;
contracts::ContractMetadata meta()
{
    contracts::ContractMetadata m;
    m.identity.tenant_id = "t";
    m.identity.task_id = "task";
    return m;
}
SkillPlanNode node(std::string id, EffectClass effect = EffectClass::ReadOnly)
{
    SkillPlanNode n;
    n.node_id = std::move(id);
    n.resolved_skill_id = n.node_id + "-skill";
    n.resolved_skill_version = "1";
    n.resolved_skill_digest = "sha256:" + n.node_id;
    n.effect = effect;
    n.approval_required = effect != EffectClass::ReadOnly;
    n.requested_permissions.tools = {"tool"};
    return n;
}
int main()
{
    AgentTemplate t;
    t.metadata = meta();
    t.template_id = "template";
    t.permissions.tools = {"tool"};
    SkillCollaborationPlan base;
    base.metadata = meta();
    base.plan_id = "plan";
    base.nodes = {node("required")};
    base.nodes[0].required = true;
    t.workflow_skeleton = base;
    PlanningContext c{meta(), t, {}, t.permissions, t.budgets, 1};
    DirectiveSkillPlanProvider directive;
    auto p = directive.propose(c, {});
    SkillCollaborationPlanValidator validator;
    assert(validator.validate(t, p, t.permissions, t.budgets, nullptr, 1).ok);
    auto cyclic = p;
    cyclic.nodes.push_back(node("second"));
    cyclic.edges = {{"required", "second", {}}, {"second", "required", {}}};
    assert(!validator.validate(t, cyclic, t.permissions, t.budgets).ok);
    auto write = p;
    write.nodes[0].effect = EffectClass::Write;
    write.nodes[0].approval_required = false;
    assert(!validator.validate(t, write, t.permissions, t.budgets).ok);
    auto escalate = p;
    escalate.nodes[0].requested_permissions.tools.push_back("danger");
    assert(!validator.validate(t, escalate, t.permissions, t.budgets).ok);
    auto removed = p;
    removed.nodes.clear();
    assert(!validator.validate(t, removed, t.permissions, t.budgets).ok);
    p.committed_effect_receipts = {"receipt-1"};
    auto revised = directive.revise(c, p, {}, {});
    revised.committed_effect_receipts.clear();
    assert(!validator.validate(t, revised, t.permissions, t.budgets, &p).ok);
    revised.committed_effect_receipts = p.committed_effect_receipts;
    assert(validator.validate(t, revised, t.permissions, t.budgets, &p).ok);
    ModelPlanCallback model = [](const PlanningContext &c, const std::optional<SkillCollaborationPlan> &prior, const nlohmann::json &, const std::vector<SkillCandidate> &)
    {SkillCollaborationPlan p;p.metadata=c.metadata;p.plan_id="model";p.revision=prior?prior->revision+1:1;p.nodes={node("model-node")};return p; };
    HybridSkillPlanProvider hybrid(model);
    auto hp = hybrid.propose(c, {});
    assert(hp.nodes.size() == 2);
    assert(std::any_of(hp.nodes.begin(), hp.nodes.end(), [](const auto &n)
                       { return n.node_id == "required"; }));
    std::cout << "test_agent_template_planning: ok\n";
}
