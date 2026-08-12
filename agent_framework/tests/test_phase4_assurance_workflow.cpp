#include <algorithm>
#include <cassert>
#include <set>

#include "phase4_assurance_workflow_test_support.hpp"

int main() {
    using namespace phase4_assurance_test;
    auto value = contract("task-f4v-workflow");
    memory_v2::MemoryProviderRegistry providers;
    memory_v2::MemoryViewEngine views(providers);
    OracleRegistry oracles;
    register_manifest_oracle(oracles);
    InMemoryAssuranceStore store;
    ScriptedAssuranceModel model;
    script_success(model);
    ProfessionalAssuranceWorkflow workflow(views, oracles, store, model);
    AssuranceWorkflowOptions options;
    options.workflow_id = "workflow-f4v-success";
    options.max_stage_attempts = 1;
    options.forbidden_independence_groups = {"executor-group"};
    options.now = [] { return "2026-08-10T00:00:00Z"; };
    std::vector<AssuranceWorkflowEvent> events;
    options.event_sink = [&](const auto& event) { events.push_back(event); };
    const auto result = workflow.run(value, subject(value.metadata), task_context(),
                                     manifest(value), options);
    assert(result.state == AssuranceWorkflowState::Completed);
    assert(result.report && result.report->decision == AcceptanceDecision::Accepted);
    assert(result.verification_plan && result.verification_plan->assignments.size() == 5);
    assert(result.checkpoint.evidence.size() == 10);
    assert(result.checkpoint.artifacts.size() == 7);
    assert(model.requests.size() == 7);
    std::set<std::string> groups;
    for(const auto& artifact : result.checkpoint.artifacts)
        assert(groups.insert(artifact.independence_group).second);
    for(const auto& request : model.requests) {
        assert(std::find(request.granted_capabilities.begin(),
                         request.granted_capabilities.end(), "repo_write") ==
               request.granted_capabilities.end());
    }
    assert(std::count_if(events.begin(), events.end(), [](const auto& event) {
        return event.event_type == "stage_completed";
    }) == 9);
    const auto durable = store.load_report("tenant-a", options.workflow_id);
    assert(durable && durable->report.decision == AcceptanceDecision::Accepted);
    const auto repeated = workflow.run(value, subject(value.metadata), task_context(),
                                       manifest(value), options);
    assert(repeated.report && repeated.report->decision == AcceptanceDecision::Accepted);
    assert(model.requests.size() == 7);

    // A model opinion cannot satisfy a mandatory criterion even when it says pass.
    {
        AcceptanceContract weak_contract=value;
        EvidenceLedger weak;
        weak.append({"weak","functional","test","model://weak","sha256:weak",
            "2026-08-10T00:00:00Z","2027-01-01T00:00:00Z",
            OracleStrength::CalibratedModel,FindingOutcome::Pass,true});
        AcceptanceArbiter arbiter;
        const auto report=arbiter.decide(weak_contract,weak,{weak_contract.metadata,
            "sha256:artifact","snapshot","view","2026-08-10T00:00:00Z"});
        assert(report.decision==AcceptanceDecision::ManualReview);
    }

    // Two independent roles may inspect the same criterion; equivalent findings are
    // normalized by criterion/outcome while retaining the union of their evidence.
    {
        auto overlap_value = contract("task-f4v-overlap");
        memory_v2::MemoryProviderRegistry overlap_providers;
        memory_v2::MemoryViewEngine overlap_views(overlap_providers);
        OracleRegistry overlap_oracles;
        register_manifest_oracle(overlap_oracles);
        InMemoryAssuranceStore overlap_store;
        ScriptedAssuranceModel overlap_model;
        auto plan = planner_output();
        plan["assignments"][1]["criterion_ids"].push_back("functional");
        plan["assignments"][1]["required_evidence"].push_back("test");
        overlap_model.push(AssuranceStage::Planning, plan,
                           "planner-provider", "planner-model", "planner-group");
        overlap_model.push(AssuranceStage::CodeVerification,
                           verifier_output(AssuranceStage::CodeVerification),
                           "code-provider", "code-model", "code-group");
        auto architecture = verifier_output(AssuranceStage::ArchitectureVerification);
        architecture["findings"].push_back({
            {"criterion_id", "functional"}, {"outcome", "pass"}, {"confidence", 0.9},
            {"evidence_ids", {"evidence:functional:test"}}, {"remediation", "none"}});
        overlap_model.push(AssuranceStage::ArchitectureVerification, architecture,
                           "architecture-provider", "architecture-model", "architecture-group");
        int index = 0;
        for(const auto stage : {AssuranceStage::DomainVerification,
                                AssuranceStage::SecurityVerification,
                                AssuranceStage::CompletenessVerification}) {
            const auto label = std::to_string(++index);
            overlap_model.push(stage, verifier_output(stage), "provider-" + label,
                               "model-" + label, "group-" + label);
        }
        overlap_model.push(AssuranceStage::EvidenceResolution, resolver_output(),
                           "resolver-provider", "resolver-model", "resolver-group");
        ProfessionalAssuranceWorkflow overlap_workflow(
            overlap_views, overlap_oracles, overlap_store, overlap_model);
        auto overlap_options = options;
        overlap_options.workflow_id = "workflow-f4v-overlap";
        overlap_options.event_sink = {};
        const auto overlap = overlap_workflow.run(
            overlap_value, subject(overlap_value.metadata), task_context(),
            manifest(overlap_value), overlap_options);
        assert(overlap.report && overlap.report->decision == AcceptanceDecision::Accepted);
        assert(overlap.resolution);
        assert(std::count_if(overlap.resolution->normalized_findings.begin(),
            overlap.resolution->normalized_findings.end(), [](const auto& finding) {
                return finding.criterion_id == "functional" &&
                       finding.outcome == FindingOutcome::Pass;
            }) == 1);
    }
    return 0;
}
