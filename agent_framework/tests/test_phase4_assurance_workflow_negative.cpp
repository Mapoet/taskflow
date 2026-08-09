#include <cassert>

#include "phase4_assurance_workflow_test_support.hpp"

namespace {
using namespace phase4_assurance_test;

AssuranceWorkflowResult run_case(const AcceptanceContract& value, json artifact_manifest,
                                 ScriptedAssuranceModel& model, std::string id) {
    memory_v2::MemoryProviderRegistry providers;
    memory_v2::MemoryViewEngine views(providers);
    OracleRegistry oracles;
    register_manifest_oracle(oracles);
    InMemoryAssuranceStore store;
    ProfessionalAssuranceWorkflow workflow(views, oracles, store, model);
    AssuranceWorkflowOptions options;
    options.workflow_id = std::move(id);
    options.max_stage_attempts = 1;
    options.forbidden_independence_groups = {"executor-group"};
    options.now = [] { return "2026-08-10T00:00:00Z"; };
    return workflow.run(value, subject(value.metadata), task_context(), artifact_manifest, options);
}
}

int main() {
    using namespace phase4_assurance_test;

    // A calibrated model PASS cannot override a deterministic metric failure.
    {
        auto value = contract("task-f4v-metric-fail");
        ScriptedAssuranceModel model;
        script_success(model);
        const auto result = run_case(value, manifest(value, false), model, "f4v-metric-fail");
        assert(result.report && result.report->decision == AcceptanceDecision::Rejected);
    }

    // Existing tests pass, but a requested system artifact is absent: no false completion.
    {
        auto value = contract("task-f4v-artifact-missing");
        ScriptedAssuranceModel model;
        script_success(model);
        const auto result = run_case(value, manifest(value, true, false), model,
                                     "f4v-artifact-missing");
        assert(result.state == AssuranceWorkflowState::ManualReview);
        assert(!result.report || result.report->decision != AcceptanceDecision::Accepted);
    }

    // A verifier cannot cite tenant-b or otherwise unknown evidence.
    {
        auto value = contract("task-f4v-evidence-closure");
        ScriptedAssuranceModel model;
        script_success(model, true);
        const auto result = run_case(value, manifest(value), model, "f4v-evidence-closure");
        assert(result.state == AssuranceWorkflowState::ManualReview);
        assert(result.error_code == "professional_verifier_invalid" ||
               result.checkpoint.error_code == "professional_verifier_invalid");
    }

    // The LLM planner cannot grant write capabilities to verification roles.
    {
        auto value = contract("task-f4v-write-capability");
        ScriptedAssuranceModel model;
        script_success(model, false, true);
        const auto result = run_case(value, manifest(value), model, "f4v-write-capability");
        assert(result.state == AssuranceWorkflowState::ManualReview);
        assert(result.error_code == "verification_plan_invalid");
    }

    // Planner and verifier self-review through the same independence group is rejected.
    {
        auto value = contract("task-f4v-self-review");
        ScriptedAssuranceModel model;
        model.push(AssuranceStage::Planning, planner_output(),
                   "planner-provider", "planner-model", "shared-group");
        model.push(AssuranceStage::CodeVerification,
                   verifier_output(AssuranceStage::CodeVerification),
                   "code-provider", "code-model", "shared-group");
        for(const auto stage : {AssuranceStage::ArchitectureVerification,
                                AssuranceStage::DomainVerification,
                                AssuranceStage::SecurityVerification,
                                AssuranceStage::CompletenessVerification}) {
            const auto label = criterion_for_stage(stage);
            model.push(stage, verifier_output(stage), label + "-provider", label + "-model",
                       label + "-group");
        }
        model.push(AssuranceStage::EvidenceResolution, resolver_output(),
                   "resolver-provider", "resolver-model", "resolver-group");
        const auto result = run_case(value, manifest(value), model, "f4v-self-review");
        assert(result.state == AssuranceWorkflowState::ManualReview);
        assert(!result.report || result.report->decision != AcceptanceDecision::Accepted);
    }

    // Executor-produced self-claims are retained for audit but cannot satisfy required evidence.
    {
        auto value = contract("task-f4v-executor-claim");
        auto artifacts = manifest(value);
        artifacts["observations"][0]["producer_kind"] = "executor";
        artifacts["canonical_digest"] = contracts::embedded_digest(artifacts).value();
        ScriptedAssuranceModel model;
        script_success(model);
        const auto result = run_case(value, artifacts, model, "f4v-executor-claim");
        assert(result.report && result.report->decision == AcceptanceDecision::ManualReview);
    }

    // Equal-strength contradictory facts are exposed for manual review, not majority-voted away.
    {
        auto value = contract("task-f4v-equal-conflict");
        auto artifacts = manifest(value);
        auto counter = observation("metric", "metric", "fail");
        counter["evidence_id"] = "evidence:metric:counter";
        artifacts["observations"].push_back(counter);
        artifacts["canonical_digest"] = contracts::embedded_digest(artifacts).value();
        ScriptedAssuranceModel model;
        script_success(model);
        const auto result = run_case(value, artifacts, model, "f4v-equal-conflict");
        assert(result.report && result.report->decision == AcceptanceDecision::ManualReview);
        assert(result.resolution && !result.resolution->conflicts.empty());
        assert(!result.resolution->conflicts.front().resolved);
    }
    return 0;
}
