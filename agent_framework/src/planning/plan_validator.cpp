#include "agent/planning/plan_validator.hpp"

#include <functional>
#include <map>
#include <set>

namespace agent_framework::planning {

bool PlanValidationResult::valid() const noexcept {
    for(const auto& issue : issues)
        if(issue.severity == PlanIssueSeverity::Error) return false;
    return true;
}

PlanValidationResult PlanValidator::validate(const ExecutionPlan& plan) const {
    PlanValidationResult out;
    auto issue = [&](std::string code, std::string node, std::string message) {
        out.issues.push_back({PlanIssueSeverity::Error, std::move(code),
                              std::move(node), std::move(message)});
    };
    if(plan.metadata.identity.tenant_id.empty() || plan.metadata.identity.task_id.empty() ||
       plan.metadata.identity.plan_id.empty())
        issue("identity_missing", {}, "tenant, task, and plan identity are required");
    if(plan.nodes.empty()) issue("plan_empty", {}, "plan must contain at least one node");
    if(plan.evidence_bundle_digest.empty() || plan.task_understanding_digest.empty() ||
       plan.acceptance_contract_digest.empty() || plan.memory_snapshot_id.empty() ||
       plan.planning_view_digest.empty())
        issue("snapshot_binding_missing", {}, "plan must bind evidence, understanding, acceptance, and memory views");
    std::map<std::string, const PlanNode*> nodes;
    for(const auto& node : plan.nodes) {
        if(node.node_id.empty() || !nodes.emplace(node.node_id, &node).second)
            issue("node_id_invalid", node.node_id, "node ids must be non-empty and unique");
        if(node.objective.empty() || node.output_contracts.empty() || node.acceptance_contract_id.empty())
            issue("node_not_executable", node.node_id, "objective, output contract, and acceptance contract are required");
        if(!node.side_effects.empty() && node.rollback_strategy.empty())
            issue("rollback_missing", node.node_id, "side-effecting node requires rollback strategy");
        if((node.risk_level == "high" || node.risk_level == "critical") && !node.approval_required)
            issue("approval_missing", node.node_id, "high-risk node requires approval");
    }
    for(const auto& node : plan.nodes)
        for(const auto& dependency : node.dependencies)
            if(!nodes.count(dependency)) issue("dependency_missing", node.node_id, dependency);

    enum class Mark { Visiting, Done };
    std::map<std::string, Mark> marks;
    std::function<void(const PlanNode&)> visit = [&](const PlanNode& node) {
        const auto found = marks.find(node.node_id);
        if(found != marks.end()) {
            if(found->second == Mark::Visiting) issue("dependency_cycle", node.node_id, "plan DAG contains a cycle");
            return;
        }
        marks[node.node_id] = Mark::Visiting;
        for(const auto& dependency : node.dependencies) {
            const auto target = nodes.find(dependency);
            if(target != nodes.end()) visit(*target->second);
        }
        marks[node.node_id] = Mark::Done;
    };
    for(const auto& node : plan.nodes) visit(node);
    for(const auto& id : plan.critical_path)
        if(!nodes.count(id)) issue("critical_path_invalid", id, "critical path references unknown node");
    return out;
}

}  // namespace agent_framework::planning
