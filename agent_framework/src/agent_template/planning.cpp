#include "agent/agent_template/planning.hpp"

#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>

namespace agent_framework::agent_template
{
    namespace
    {
        void issue(PlanValidationResult &o, std::string c, std::string p, std::string m) { o.issues.push_back({std::move(c), std::move(p), std::move(m)}); }
        SkillCollaborationPlan skeleton(const PlanningContext &c)
        {
            if (!c.agent_template.workflow_skeleton)
                throw std::invalid_argument("template workflow skeleton required");
            auto p = *c.agent_template.workflow_skeleton;
            p.metadata = c.metadata;
            p.budget = c.budget;
            return p;
        }
        void resolve_open_nodes(SkillCollaborationPlan &p, const std::vector<SkillCandidate> &cs)
        {
            for (auto &n : p.nodes)
                if (!n.resolved_skill_id)
                {
                    auto it = std::find_if(cs.begin(), cs.end(), [&](const auto &c)
                                           { return n.selector.candidates.empty() || std::find(n.selector.candidates.begin(), n.selector.candidates.end(), c.skill_id) != n.selector.candidates.end(); });
                    if (it != cs.end())
                    {
                        n.resolved_skill_id = it->skill_id;
                        n.resolved_skill_version = it->version;
                        n.resolved_skill_digest = it->package_digest;
                    }
                }
        }
    }
    SkillCollaborationPlan FixedWorkflowPlanProvider::propose(const PlanningContext &c, const std::vector<SkillCandidate> &) { return skeleton(c); }
    SkillCollaborationPlan FixedWorkflowPlanProvider::revise(const PlanningContext &, const SkillCollaborationPlan &p, const nlohmann::json &, const std::vector<SkillCandidate> &) { return p; }
    SkillCollaborationPlan DirectiveSkillPlanProvider::propose(const PlanningContext &c, const std::vector<SkillCandidate> &cs)
    {
        auto p = skeleton(c);
        resolve_open_nodes(p, cs);
        return p;
    }
    SkillCollaborationPlan DirectiveSkillPlanProvider::revise(const PlanningContext &, const SkillCollaborationPlan &p, const nlohmann::json &, const std::vector<SkillCandidate> &cs)
    {
        auto out = p;
        out.parent_digest = encode(p).at("canonical_digest");
        ++out.revision;
        resolve_open_nodes(out, cs);
        return out;
    }
    SkillCollaborationPlan ModelSkillPlanProvider::propose(const PlanningContext &c, const std::vector<SkillCandidate> &cs)
    {
        if (!callback_)
            throw std::invalid_argument("model plan callback required");
        return callback_(c, std::nullopt, nlohmann::json::object(), cs);
    }
    SkillCollaborationPlan ModelSkillPlanProvider::revise(const PlanningContext &c, const SkillCollaborationPlan &p, const nlohmann::json &o, const std::vector<SkillCandidate> &cs)
    {
        if (!callback_)
            throw std::invalid_argument("model plan callback required");
        return callback_(c, p, o, cs);
    }
    SkillCollaborationPlan HybridSkillPlanProvider::propose(const PlanningContext &c, const std::vector<SkillCandidate> &cs)
    {
        auto model = model_.propose(c, cs);
        if (!c.agent_template.workflow_skeleton)
            return model;
        const auto &base = *c.agent_template.workflow_skeleton;
        for (const auto &required : base.nodes)
            if (required.required && std::none_of(model.nodes.begin(), model.nodes.end(), [&](const auto &n)
                                                  { return n.node_id == required.node_id; }))
                model.nodes.push_back(required);
        return model;
    }
    SkillCollaborationPlan HybridSkillPlanProvider::revise(const PlanningContext &c, const SkillCollaborationPlan &p, const nlohmann::json &o, const std::vector<SkillCandidate> &cs)
    {
        auto r = model_.revise(c, p, o, cs);
        if (c.agent_template.workflow_skeleton)
            for (const auto &required : c.agent_template.workflow_skeleton->nodes)
                if (required.required && std::none_of(r.nodes.begin(), r.nodes.end(), [&](const auto &n)
                                                      { return n.node_id == required.node_id; }))
                    r.nodes.push_back(required);
        return r;
    }

    PlanValidationResult SkillCollaborationPlanValidator::validate(const AgentTemplate &t, const SkillCollaborationPlan &p, const PermissionEnvelope &permissions, const BudgetPolicy &budget, const SkillCollaborationPlan *prior, std::uint64_t expected) const
    {
        PlanValidationResult out;
        if (p.plan_id.empty() || p.revision == 0)
            issue(out, "plan_identity_invalid", "/plan", "plan id and positive revision required");
        if (expected && p.revision != expected)
            issue(out, "plan_revision_conflict", "/revision", "plan revision does not match CAS expectation");
        if (p.budget.max_iterations > budget.max_iterations || p.budget.max_tool_calls > budget.max_tool_calls || p.budget.max_parallelism > budget.max_parallelism || (budget.max_tokens && p.budget.max_tokens > budget.max_tokens) || (budget.deadline_ms && p.budget.deadline_ms > budget.deadline_ms) || (budget.max_cost > 0 && p.budget.max_cost > budget.max_cost))
            issue(out, "budget_escalation", "/budget", "plan exceeds budget ceiling");
        std::map<std::string, std::size_t> ids;
        for (std::size_t i = 0; i < p.nodes.size(); ++i)
        {
            const auto &n = p.nodes[i];
            if (n.node_id.empty() || !ids.emplace(n.node_id, i).second)
                issue(out, "node_identity_invalid", "/nodes/" + std::to_string(i), "node id empty or duplicate");
            if (!n.resolved_skill_id)
                issue(out, "skill_unresolved", "/nodes/" + std::to_string(i), "resolved skill required");
            std::string reason;
            if (!permission_is_subset(n.requested_permissions, permissions, &reason))
                issue(out, "permission_escalation", "/nodes/" + std::to_string(i), reason);
            if (n.effect != EffectClass::ReadOnly && !n.approval_required)
                issue(out, "approval_required", "/nodes/" + std::to_string(i), "write or unknown effect requires approval");
            if (n.verifier && !n.output.evidence_required)
                issue(out, "verifier_evidence_required", "/nodes/" + std::to_string(i), "verifier must require evidence");
        }
        std::map<std::string, std::vector<std::string>> adj;
        std::map<std::string, int> degree;
        for (const auto &[id, _] : ids)
            degree[id] = 0;
        for (std::size_t i = 0; i < p.edges.size(); ++i)
        {
            const auto &e = p.edges[i];
            if (!ids.count(e.from) || !ids.count(e.to))
            {
                issue(out, "edge_endpoint_missing", "/edges/" + std::to_string(i), "edge endpoint missing");
                continue;
            }
            adj[e.from].push_back(e.to);
            ++degree[e.to];
        }
        std::queue<std::string> q;
        for (const auto &[id, d] : degree)
            if (d == 0)
                q.push(id);
        std::size_t seen = 0;
        while (!q.empty())
        {
            auto id = q.front();
            q.pop();
            ++seen;
            for (const auto &next : adj[id])
                if (--degree[next] == 0)
                    q.push(next);
        }
        if (seen != ids.size())
            issue(out, "plan_cycle", "/edges", "collaboration plan must be acyclic");
        if (t.workflow_skeleton)
            for (const auto &required : t.workflow_skeleton->nodes)
                if (required.required && !ids.count(required.node_id))
                    issue(out, "required_node_removed", "/nodes", "required template node removed: " + required.node_id);
        if (prior)
        {
            if (p.revision != prior->revision + 1)
                issue(out, "plan_revision_invalid", "/revision", "replan revision must increment by one");
            const auto prior_digest = encode(*prior).at("canonical_digest").get<std::string>();
            if (p.parent_digest != prior_digest)
                issue(out, "parent_digest_mismatch", "/parent_digest", "replan must bind prior digest");
            for (const auto &r : prior->committed_effect_receipts)
                if (std::find(p.committed_effect_receipts.begin(), p.committed_effect_receipts.end(), r) == p.committed_effect_receipts.end())
                    issue(out, "committed_effect_denied", "/committed_effect_receipts", "replan cannot forget committed effect");
        }
        out.ok = out.issues.empty();
        return out;
    }
} // namespace agent_framework::agent_template
