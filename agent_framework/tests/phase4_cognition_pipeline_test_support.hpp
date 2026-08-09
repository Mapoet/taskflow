#pragma once

#include <cassert>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agent/planning/cognition_pipeline.hpp"

namespace phase4_cognition_test {

using namespace agent_framework;
using namespace agent_framework::planning;
using json = nlohmann::json;

inline TaskIntake intake(std::string task_id = "task-f2c") {
    TaskIntake value;
    value.metadata.identity.tenant_id = "tenant-a";
    value.metadata.identity.organization_id = "org-a";
    value.metadata.identity.principal_id = "user-a";
    value.metadata.identity.project_id = "project-a";
    value.metadata.identity.task_id = std::move(task_id);
    value.metadata.identity.run_id = value.metadata.identity.task_id + ":run";
    value.metadata.identity.plan_id = value.metadata.identity.task_id + ":plan";
    value.user_goal = "upgrade the public workflow API without breaking callers";
    value.requested_deliverables = {"code", "tests", "migration notes"};
    value.explicit_constraints = {"preserve compatibility"};
    value.granted_authorities = {
        "workspace_read", "workspace_write", "repo_read", "external_read"};
    value.success_signals = {"all tests pass", "public callers migrated"};
    return value;
}

inline memory_v2::MemoryScope subject(const TaskIntake& value) {
    memory_v2::MemoryScope scope;
    scope.tenant_id = value.metadata.identity.tenant_id;
    scope.organization_id = value.metadata.identity.organization_id;
    scope.principal_id = value.metadata.identity.principal_id;
    scope.project_id = value.metadata.identity.project_id;
    scope.task_id = value.metadata.identity.task_id;
    scope.run_id = value.metadata.identity.run_id;
    return scope;
}

inline json intake_output(bool clarify = false) {
    return {{"goal", "upgrade workflow API"},
            {"constraints", {"preserve compatibility"}},
            {"acceptance_criteria", {"tests pass", "callers migrated"}},
            {"ambiguities", clarify ? json{"Which compatibility window?"} : json::array()},
            {"risks", {"public API breakage"}},
            {"clarification_required", clarify},
            {"clarification_questions", clarify ? json{"Which compatibility window?"}
                                                 : json::array()}};
}

inline json strategy_output() {
    return {{"fact_gaps", {"caller inventory", "official migration behavior"}},
            {"source_priorities", {"repository", "official_docs"}},
            {"steps", json::array({
                {{"investigator_id", "repo"}, {"question", "map callers"},
                 {"round", 1}, {"required", true}},
                {{"investigator_id", "docs"}, {"question", "verify migration contract"},
                 {"round", 2}, {"required", true}}})},
            {"max_tool_calls", 4},
            {"stop_conditions", {"caller and migration claims both have evidence"}}};
}

inline json synthesis_output(bool hallucinate = false) {
    return {{"claims", json::array({
                {{"claim_id", "claim-callers"}, {"statement", "two public callers exist"},
                 {"evidence_ids", {hallucinate ? "ev-missing" : "ev-repo"}},
                 {"assumption", false}, {"confidence", 0.95}},
                {{"claim_id", "claim-window"}, {"statement", "compatibility shim is prudent"},
                 {"evidence_ids", json::array()}, {"assumption", true},
                 {"confidence", 0.6}}})},
            {"conflicts", json::array()}, {"unknowns", json::array()}};
}

inline json boundary_output() {
    return {{"change_mode", "upgrade"}, {"rationale", "public contract changes"},
            {"in_scope", {"workflow API", "callers"}},
            {"out_of_scope", {"unrelated UI"}},
            {"upstream_contracts", {"TaskIntake"}},
            {"downstream_contracts", {"GraphExecutor callers"}},
            {"blast_radius", "public API and two callers"},
            {"risks", {"ABI mismatch"}}};
}

inline json plan_output(bool revised = false, bool approval_required = false) {
    return {{"understanding",
             {{"domain", "software architecture"},
              {"current_state", "legacy workflow API"},
              {"target_state", "versioned workflow API"},
              {"gaps", {"caller migration"}},
              {"assumptions", {"compatibility shim is acceptable"}},
              {"unknowns", json::array()},
              {"risks", {"ABI mismatch"}},
              {"evidence_ids", {"ev-repo", "ev-doc"}}}},
            {"plan",
             {{"acceptance_contract_digest", "sha256:acceptance-f2c"},
              {"nodes", json::array({
                  {{"node_id", "inspect"}, {"objective", "freeze current public contract"},
                   {"in_scope", {"workflow API"}}, {"out_of_scope", json::array()},
                   {"input_contracts", {"TaskIntake"}}, {"output_contracts", {"api-map"}},
                   {"dependencies", json::array()}, {"required_capabilities", {"repo_read"}},
                   {"side_effects", json::array()}, {"acceptance_contract_id", "accept-map"},
                   {"rollback_strategy", ""}, {"risk_level", "low"},
                   {"approval_required", false}, {"basis_refs", {"ev-repo"}}},
                  {{"node_id", "change"},
                   {"objective", revised ? "add shim and migrate callers" : "migrate callers"},
                   {"in_scope", {"workflow API", "callers"}},
                   {"out_of_scope", {"UI"}}, {"input_contracts", {"api-map"}},
                   {"output_contracts", {"patched-api", "migration-tests"}},
                   {"dependencies", {"inspect"}},
                   {"required_capabilities", {"repo_write"}},
                   {"side_effects", {"workspace-write"}},
                   {"acceptance_contract_id", "accept-build-test"},
                   {"rollback_strategy", "revert patch"},
                   {"risk_level", approval_required ? "high" : "medium"},
                   {"approval_required", approval_required},
                   {"basis_refs", {"ev-doc", "assumption:compatibility shim is acceptable"}}}})},
              {"critical_path", {"inspect", "change"}},
              {"budget", {{"wall_time_ms", 60000}, {"token_budget", 12000},
                           {"tool_calls", 8}, {"cost_limit", 1.0}}}}}};
}

inline json critique_output(bool approved) {
    json findings = json::array();
    if(!approved) {
        findings.push_back({{"code", "compatibility_missing"}, {"severity", "error"},
                            {"node_id", "change"},
                            {"message", "migration lacks compatibility shim"},
                            {"evidence_ids", {"ev-doc"}},
                            {"counterexample", "old caller loads new library"},
                            {"remediation", "add shim before caller migration"}});
    }
    return {{"approved", approved}, {"findings", std::move(findings)},
            {"counterexamples", approved ? json::array()
                                           : json{"old caller loads new library"}}};
}

class RepoInvestigator final : public Investigator {
public:
    std::string id() const override { return "repo"; }
    bool external() const noexcept override { return false; }
    std::vector<std::string> required_capabilities() const override { return {"repo_read"}; }
    std::vector<EvidenceRecord> investigate(const InvestigationRequest& request,
                                             std::string*) override {
        rounds.push_back(request.round);
        return {{"ev-repo", "repository", "/repo/include/workflow.hpp", "sha256:repo",
                 "2026-08-09T00:00:00Z", "direct_observation", "",
                 {"claim-callers"}, {}, false}};
    }
    std::vector<std::uint64_t> rounds;
};

class DocsInvestigator final : public Investigator {
public:
    std::string id() const override { return "docs"; }
    bool external() const noexcept override { return true; }
    std::vector<std::string> required_capabilities() const override { return {"external_read"}; }
    std::vector<EvidenceRecord> investigate(const InvestigationRequest& request,
                                             std::string*) override {
        rounds.push_back(request.round);
        return {{"ev-doc", "external", "https://example.invalid/workflow", "sha256:docs",
                 "2026-08-09T00:00:00Z", "official", "2027-01-01T00:00:00Z",
                 {"claim-window"}, {}, true}};
    }
    std::vector<std::uint64_t> rounds;
};

class ScriptedStageModel final : public CognitionStageModel {
public:
    void push(CognitionStage stage, json output,
              std::string provider = "planner-provider",
              std::string model = "planner-model",
              std::string group = "planner") {
        scripts_[stage].push_back({std::move(output), std::move(provider),
                                   std::move(model), std::move(group)});
    }

    CognitionStageResponse invoke(const CognitionStageRequest& request) override {
        requests.push_back(request);
        auto& queue = scripts_[request.stage];
        if(queue.empty()) return {false, {}, {}, "script_exhausted", cognition_stage_name(request.stage)};
        auto item = std::move(queue.front());
        queue.pop_front();
        llm_runtime::LLMInvocationManifest manifest;
        manifest.metadata = request.metadata;
        manifest.invocation_id = request.pipeline_id + ":" + cognition_stage_name(request.stage) +
                                 ":" + std::to_string(request.iteration) +
                                 ":" + std::to_string(request.attempt);
        manifest.state = llm_runtime::InvocationState::Succeeded;
        manifest.role = cognition_stage_name(request.stage);
        manifest.profile_id = manifest.role;
        manifest.profile_revision = "r1";
        manifest.prompt_id = manifest.role;
        manifest.prompt_revision = "r1";
        manifest.prompt_digest = "sha256:prompt";
        manifest.route_decision_digest = "sha256:route";
        manifest.candidate_id = item.provider;
        manifest.provider = item.provider;
        manifest.model = item.model;
        manifest.adapter_revision = "adapter-r1";
        manifest.reasoning_effort = "high";
        manifest.independence_group = item.group;
        manifest.evidence_authority = "advisory";
        manifest.memory_snapshot_id = request.memory_view.snapshot.snapshot_id;
        manifest.memory_view_profile = "test";
        manifest.memory_view_digest = request.memory_view.manifest.view_digest;
        manifest.input_digest = "sha256:input";
        manifest.output_digest = contracts::embedded_digest(item.output).value_or("");
        manifest.started_at = "2026-08-09T00:00:00Z";
        manifest.finished_at = "2026-08-09T00:00:01Z";
        return {true, std::move(item.output), std::move(manifest), {}, {}};
    }

    struct Item { json output; std::string provider; std::string model; std::string group; };
    std::map<CognitionStage, std::deque<Item>> scripts_;
    std::vector<CognitionStageRequest> requests;
};

inline void script_success(ScriptedStageModel& model, bool revision = true,
                           bool approval_required = false) {
    model.push(CognitionStage::Intake, intake_output());
    model.push(CognitionStage::Strategy, strategy_output());
    model.push(CognitionStage::Synthesis, synthesis_output());
    model.push(CognitionStage::Boundary, boundary_output());
    model.push(CognitionStage::Planning, plan_output(false, approval_required));
    model.push(CognitionStage::Critique, critique_output(!revision),
               "critic-provider", "critic-model", "critic");
    if(revision) {
        model.push(CognitionStage::Revision, plan_output(true, approval_required));
        model.push(CognitionStage::Critique, critique_output(true),
                   "critic-provider", "critic-model", "critic");
    }
}

}  // namespace phase4_cognition_test
