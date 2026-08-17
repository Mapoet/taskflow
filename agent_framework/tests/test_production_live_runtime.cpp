#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <memory>

#include "agent/runtime/production_live_runtime.hpp"
#include "agent/internal/platform_io.hpp"
#include "phase4_harness_test_support.hpp"

int main() {
    using namespace agent_framework::runtime;
    ProductionRuntimeResources empty;
    auto no_anchors = ProductionLiveRuntime::build(empty);
    assert(!no_anchors);
    assert(no_anchors.error == "production_runtime_lifetime_anchors_required");

    empty.lifetime_anchors.push_back(std::make_shared<int>(1));
    auto wrong_profile = ProductionLiveRuntime::build(empty);
    assert(!wrong_profile);
    assert(wrong_profile.error == "production_runtime_profile_required");
    empty.deployment_profile = "production";
    auto missing_owner = ProductionLiveRuntime::build(empty);
    assert(!missing_owner);
    assert(missing_owner.error ==
           "production_runtime_owned_resource_missing:conversation_store");
    for(const auto* name : {"conversation_store", "task_registry", "run_store",
             "harness_store", "plan_store", "invocation_store",
             "incremental_result_store", "effect_journal", "approval_store",
             "memory_store", "assurance_store", "judge_store", "telemetry"})
        empty.owned_resources[name] = std::make_shared<int>(1);
    const auto ownership = validate_production_runtime_ownership(empty);
    assert(ownership.ready && ownership.missing.empty());
    assert(!ownership.manifest_digest.empty());
    auto registry = std::make_shared<agent_framework::conversation::SQLiteTaskRegistry>(
        ":memory:");
    empty.dependencies.task_registry = registry.get();
    const auto mismatched = validate_production_runtime_ownership(empty);
    assert(!mismatched.ready && mismatched.mismatched.size() == 1);
    assert(mismatched.mismatched.front() == "task_registry");
    empty.owned_resources["task_registry"] = registry;
    empty.lifetime_anchors.push_back(registry);
    assert(validate_production_runtime_ownership(empty).ready);
    auto missing = ProductionLiveRuntime::build(std::move(empty));
    assert(!missing);
    assert(missing.error.find("production_dependency_missing") != std::string::npos);

    using namespace agent_framework;
    const auto root = std::filesystem::temp_directory_path() /
        ("production-closure-" + std::to_string(internal::current_process_id()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    harness::SQLiteProductionWorkflowInputRepository inputs(
        (root / "inputs.sqlite3").string());
    harness::InMemoryHarnessStore harness_store;
    auto counters = std::make_shared<phase4_harness_test::PortCounters>();
    harness::Phase4HarnessRuntime harness_runtime(
        harness_store, phase4_harness_test::ports(counters, false));
    auto completed = harness_runtime.run(
        phase4_harness_test::start("closure-harness"));
    assert(completed.state == harness::HarnessState::Completed);

    assurance::AcceptanceContract acceptance;
    acceptance.metadata = completed.checkpoint.metadata;
    acceptance.revision = 1;
    acceptance.plan_digest = completed.checkpoint.pins.plan_digest;
    acceptance.criteria = {{"criterion-a", assurance::VerificationLayer::System,
        "verified in the real system", "repository", {"repository"}, "pass", true}};
    const auto acceptance_document = assurance::encode(acceptance);
    const auto acceptance_digest =
        acceptance_document.at("canonical_digest").get<std::string>();
    assert(inputs.put_acceptance_contract(acceptance));

    assurance::AcceptanceReport report;
    report.metadata = completed.checkpoint.metadata;
    report.plan_digest = completed.checkpoint.pins.plan_digest;
    report.acceptance_contract_digest = acceptance_digest;
    report.artifact_manifest_digest =
        completed.checkpoint.pins.artifact_manifest_digest;
    report.memory_snapshot_id = completed.checkpoint.pins.memory_snapshot_id;
    report.verification_view_digest = completed.checkpoint.pins.memory_view_digest;
    report.decision = assurance::AcceptanceDecision::Accepted;
    const auto report_digest = contracts::canonical_digest(
        assurance::encode(report)).value_or("");
    assert(!report_digest.empty());
    assert(inputs.put_acceptance_report(report, report_digest));

    assurance::AssuranceCheckpoint assurance_checkpoint;
    assurance_checkpoint.metadata = completed.checkpoint.metadata;
    assurance_checkpoint.workflow_id = "closure-assurance";
    assurance_checkpoint.revision = completed.checkpoint.revision;
    assurance_checkpoint.state = assurance::AssuranceWorkflowState::Completed;
    assurance_checkpoint.next_stage = assurance::AssuranceStage::Complete;
    assurance_checkpoint.acceptance_contract_digest = acceptance_digest;
    assurance_checkpoint.task_context_digest = "sha256:task-context";
    assurance_checkpoint.artifact_manifest_digest =
        completed.checkpoint.pins.artifact_manifest_digest;
    assurance_checkpoint.memory_snapshot_id =
        completed.checkpoint.pins.memory_snapshot_id;
    assurance_checkpoint.memory_view_digest =
        completed.checkpoint.pins.memory_view_digest;
    assurance_checkpoint.acceptance_report_digest = report_digest;
    assurance_checkpoint.updated_at = "2026-08-16T00:00:00Z";
    assurance_checkpoint.evidence = {{"evidence-a", "criterion-a", "repository",
        "file://workspace/result", "sha256:evidence-a", "2026-08-16T00:00:00Z",
        "2026-08-17T00:00:00Z", assurance::OracleStrength::RealSystem,
        assurance::FindingOutcome::Pass, true}};
    assert(inputs.put_assurance_checkpoint(assurance_checkpoint, report_digest));
    completed.checkpoint.pins.acceptance_report_digest = report_digest;

    const auto unavailable = evaluate_production_task_closure(
        inputs, completed.checkpoint.metadata.identity, "sha256:missing",
        conversation::TaskExecutionProfile::ArtifactDelivery,
        completed.checkpoint);
    assert(unavailable.state == harness::TaskTerminalState::ManualReview);
    const auto verified = evaluate_production_task_closure(
        inputs, completed.checkpoint.metadata.identity, acceptance_digest,
        conversation::TaskExecutionProfile::ArtifactDelivery,
        completed.checkpoint);
    assert(verified.state == harness::TaskTerminalState::CompletedVerified);
    std::filesystem::remove_all(root, ec);
}
