#pragma once

#include <deque>
#include <map>
#include <stdexcept>

#include "agent/remediation/remediation_workflow.hpp"

namespace phase4_remediation_test {
using namespace agent_framework;
using namespace agent_framework::remediation;
using json = nlohmann::json;

inline contracts::ContractMetadata metadata(std::string task = "task-f5r") {
    contracts::ContractMetadata v;
    v.identity.tenant_id = "tenant-a"; v.identity.organization_id = "org-a";
    v.identity.principal_id = "user-a"; v.identity.project_id = "project-a";
    v.identity.task_id = std::move(task); v.identity.run_id = v.identity.task_id + ":run";
    v.identity.plan_id = v.identity.task_id + ":plan";
    return v;
}
inline memory_v2::MemoryScope subject(const contracts::ContractMetadata& m) {
    memory_v2::MemoryScope s;
    s.tenant_id = m.identity.tenant_id; s.organization_id = m.identity.organization_id;
    s.principal_id = m.identity.principal_id; s.project_id = m.identity.project_id;
    s.task_id = m.identity.task_id; s.run_id = m.identity.run_id;
    s.level = memory_v2::MemoryLevel::Task; return s;
}
inline planning::ExecutionPlan execution_plan(std::string task = "task-f5r") {
    planning::ExecutionPlan p;
    p.metadata = metadata(std::move(task)); p.task_understanding_digest = "sha256:understanding";
    p.evidence_bundle_digest = "sha256:evidence"; p.acceptance_contract_digest = "sha256:acceptance";
    p.memory_snapshot_id = "snapshot-planning"; p.planning_view_digest = "sha256:planning-view";
    p.nodes = {{"build", "build the target", {"src"}, {}, {"source"}, {"binary"}, {},
                {"repo_read"}, {}, "functional", {}, "low", false}};
    p.critical_path = {"build"}; p.budget = {60000, 100000, 100, 10.0}; return p;
}
inline assurance::AcceptanceContract contract(const planning::ExecutionPlan& p) {
    assurance::AcceptanceContract c; c.metadata = p.metadata;
    c.plan_digest = planning::encode(p).at("canonical_digest").get<std::string>();
    c.criteria = {{"functional", assurance::VerificationLayer::Functional,
                   "binary passes tests", "test", {"test"}, "pass", true},
                  {"system", assurance::VerificationLayer::System,
                   "package is complete", "artifact", {"artifact"}, "pass", true}};
    return c;
}
inline assurance::AcceptanceReport report(const planning::ExecutionPlan& p,
                                           const assurance::AcceptanceContract& c) {
    assurance::AcceptanceReport r; r.metadata = p.metadata;
    r.plan_digest = planning::encode(p).at("canonical_digest").get<std::string>();
    r.acceptance_contract_digest = assurance::encode(c).at("canonical_digest").get<std::string>();
    r.artifact_manifest_digest = "sha256:artifact-manifest";
    r.memory_snapshot_id = "snapshot-verification"; r.verification_view_digest = "sha256:verify-view";
    r.findings = {{"finding-functional", "functional", "high", assurance::FindingOutcome::Fail,
                   0.99, {"evidence-test"}, "repair the source and rerun tests"}};
    r.decision = assurance::AcceptanceDecision::Rejected; return r;
}
inline assurance::AssuranceCheckpoint assurance_checkpoint(
    const planning::ExecutionPlan& p, const assurance::AcceptanceContract& c,
    const assurance::AcceptanceReport& r) {
    assurance::AssuranceCheckpoint a; a.metadata = p.metadata; a.workflow_id = "assurance-f5r";
    a.state = assurance::AssuranceWorkflowState::Completed; a.next_stage = assurance::AssuranceStage::Complete;
    a.acceptance_contract_digest = assurance::encode(c).at("canonical_digest").get<std::string>();
    a.task_context_digest = "sha256:task"; a.artifact_manifest_digest = r.artifact_manifest_digest;
    a.memory_snapshot_id = r.memory_snapshot_id; a.memory_view_digest = r.verification_view_digest;
    a.acceptance_report_digest = assurance::encode(r).at("canonical_digest").get<std::string>();
    a.updated_at = "2026-08-10T00:00:00Z"; return a;
}
inline ImpactInventory inventory(const planning::ExecutionPlan& p,
                                 const assurance::AcceptanceReport& r) {
    ImpactInventory i; i.metadata = p.metadata; i.inventory_id = "inventory-f5r";
    i.plan_digest = planning::encode(p).at("canonical_digest").get<std::string>();
    i.artifact_manifest_digest = r.artifact_manifest_digest;
    i.requirements = {{"req-functional", {"functional"}, {"build"}, {"binary"}}};
    i.artifacts = {{"binary", "sha256:binary-old", "build", {"functional"}, {},
                    {"evidence-test"}, {"memory-build"}, {"code"}},
                   {"package", "sha256:package-old", "build", {"system"}, {"binary"},
                    {"evidence-artifact"}, {"memory-package"}, {"completeness"}},
                   {"docs", "sha256:docs", "build", {"system"}, {},
                    {"evidence-docs"}, {}, {"completeness"}}};
    i.evidence = {{"evidence-test", "functional", "binary", "test", "sha256:test-old",
                   "2027-01-01T00:00:00Z", true},
                  {"evidence-artifact", "system", "package", "artifact", "sha256:artifact-old",
                   "2027-01-01T00:00:00Z", true},
                  {"evidence-docs", "system", "docs", "artifact", "sha256:docs-evidence",
                   "2027-01-01T00:00:00Z", true}};
    return i;
}
inline json impact_output(std::string extra = {}) {
    json artifacts = json::array(); if(!extra.empty()) artifacts.push_back(std::move(extra));
    return {{"additional_plan_node_ids", json::array()}, {"additional_artifact_ids", std::move(artifacts)},
            {"rationale", "inventory-bound downstream impact"}};
}
inline json planner_output(bool criterion_downgrade = false, bool bad_capability = false) {
    json changes = json::array();
    if(criterion_downgrade) changes.push_back({{"criterion_id", "functional"},
        {"new_mandatory", false}, {"new_threshold", "best_effort"}, {"rationale", "ship sooner"}});
    json actions = json::array({{{"action_id", "repair-functional"},
        {"objective", "repair the failed functional behavior"}, {"finding_ids", {"finding-functional"}},
        {"affected_plan_node_ids", {"build"}}, {"affected_artifact_ids", {"binary"}},
        {"required_capabilities", {bad_capability ? "network_admin" : "repo_write"}},
        {"side_effects", {"workspace_write"}}, {"output_contracts", {"binary"}},
        {"rollback_strategy", "restore the pre-repair artifact digest"},
        {"risk_level", "medium"}, {"approval_required", true}}});
    return {{"actions", std::move(actions)}, {"criterion_changes", std::move(changes)}};
}
inline json reverify_output() {
    return {{"criterion_ids", {"functional"}}, {"reuse_evidence_ids", {"evidence-test", "evidence-docs"}},
            {"verifier_roles", {"code"}}};
}

class ScriptedModel final : public RemediationStageModel {
public:
    void push(RemediationStage stage, json output) { scripts[stage].push_back(std::move(output)); }
    RemediationStageResponse invoke(const RemediationStageRequest& request) override {
        requests.push_back(request);
        auto& queue = scripts[request.stage];
        if(queue.empty()) return {false, {}, {}, "script_exhausted", remediation_stage_name(request.stage)};
        auto output = std::move(queue.front()); queue.pop_front();
        llm_runtime::LLMInvocationManifest m; m.metadata = request.metadata;
        m.invocation_id = request.workflow_id + ":" + remediation_stage_name(request.stage) +
                          ":" + std::to_string(request.attempt);
        m.state = llm_runtime::InvocationState::Succeeded; m.provider = "offline-provider";
        m.model = remediation_stage_name(request.stage) + "-model";
        m.independence_group = remediation_stage_name(request.stage) + "-group";
        m.output_digest = contracts::embedded_digest(output).value();
        m.usage.input_tokens = 100; m.usage.output_tokens = 50; m.usage.cost_usd = 0.01;
        return {true, std::move(output), std::move(m), {}, {}};
    }
    std::map<RemediationStage, std::deque<json>> scripts;
    std::vector<RemediationStageRequest> requests;
};
inline void script_success(ScriptedModel& model, bool downgrade = false) {
    model.push(RemediationStage::ImpactAnalysis, impact_output());
    model.push(RemediationStage::RemediationPlanning, planner_output(downgrade));
    model.push(RemediationStage::ReverificationPlanning, reverify_output());
}
inline RemediationWorkflowOptions options(std::string id = "workflow-f5r") {
    RemediationWorkflowOptions o; o.workflow_id = std::move(id); o.max_iterations = 2;
    o.allowed_capabilities = {"repo_read", "repo_write"}; o.actor_id = "agent-executor";
    o.approval_decision_id = "decision-f5r";
    o.approval_validator = [](std::string_view request, std::string_view decision) {
        return request.rfind("sha256:", 0) == 0 && decision == "decision-f5r";
    };
    o.now = [] { return "2026-08-10T00:00:00Z"; }; return o;
}
}  // namespace phase4_remediation_test
