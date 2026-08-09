#include <cassert>
#include <chrono>
#include <filesystem>

#include "phase4_assurance_workflow_test_support.hpp"

namespace {
using namespace phase4_assurance_test;

VerificationPlan sample_plan(const AcceptanceContract& value) {
    VerificationPlan plan;
    plan.metadata = value.metadata;
    plan.verification_plan_id = "workflow-store:plan";
    plan.acceptance_contract_digest = encode(value).at("canonical_digest").get<std::string>();
    plan.task_plan_digest = value.plan_digest;
    plan.artifact_manifest_digest = "sha256:artifact";
    plan.planner_invocation_id = "planner:1";
    plan.created_at = "2026-08-10T00:00:00Z";
    plan.assignments = {{"verify-code", ProfessionalRole::Code, {"functional"}, {"test"},
                         {"repo_read"}, true, true, "independent review"}};
    return plan;
}

AssuranceCheckpoint sample_checkpoint(const AcceptanceContract& value) {
    AssuranceCheckpoint checkpoint;
    checkpoint.metadata = value.metadata;
    checkpoint.workflow_id = "workflow-store";
    checkpoint.acceptance_contract_digest = encode(value).at("canonical_digest").get<std::string>();
    checkpoint.task_context_digest = "sha256:task-context";
    checkpoint.artifact_manifest_digest = "sha256:artifact";
    checkpoint.memory_snapshot_id = "snapshot-a";
    checkpoint.memory_view_digest = "sha256:view-a";
    checkpoint.verification_plan = sample_plan(value);
    checkpoint.updated_at = "2026-08-10T00:00:00Z";
    return checkpoint;
}

AcceptanceReport sample_report(const AcceptanceContract& value,
                               const AssuranceCheckpoint& checkpoint) {
    AcceptanceReport report;
    report.metadata = value.metadata;
    report.plan_digest = value.plan_digest;
    report.acceptance_contract_digest = checkpoint.acceptance_contract_digest;
    report.artifact_manifest_digest = checkpoint.artifact_manifest_digest;
    report.memory_snapshot_id = checkpoint.memory_snapshot_id;
    report.verification_view_digest = checkpoint.memory_view_digest;
    report.decision = AcceptanceDecision::Accepted;
    return report;
}
}

int main() {
    using namespace phase4_assurance_test;
    const auto value = contract("task-f4v-contracts");
    const auto plan = sample_plan(value);
    const auto encoded_plan = encode(plan);
    assert(decode_verification_plan(encoded_plan));
    auto tampered = encoded_plan;
    tampered["payload"]["revision"] = 9;
    assert(!decode_verification_plan(tampered));
    auto nested_unknown = encoded_plan;
    nested_unknown["payload"]["assignments"][0]["unexpected"] = true;
    nested_unknown["canonical_digest"] = contracts::embedded_digest(nested_unknown).value();
    assert(!decode_verification_plan(nested_unknown));

    const auto checkpoint = sample_checkpoint(value);
    assert(decode_assurance_checkpoint(encode(checkpoint)));
    auto checkpoint_unknown = encode(checkpoint);
    checkpoint_unknown["payload"]["unknown"] = 1;
    checkpoint_unknown["canonical_digest"] = contracts::embedded_digest(checkpoint_unknown).value();
    assert(!decode_assurance_checkpoint(checkpoint_unknown));

    InMemoryAssuranceStore memory;
    auto created = memory.create_checkpoint(checkpoint);
    assert(created && created.revision == 1);
    assert(memory.create_checkpoint(checkpoint).status == AssuranceStoreStatus::AlreadyExists);
    auto revised = checkpoint;
    revised.revision = 2;
    revised.next_stage = AssuranceStage::DeterministicEvidence;
    assert(memory.compare_exchange_checkpoint(revised, 1));
    auto stale = revised;
    stale.revision = 2;
    assert(memory.compare_exchange_checkpoint(stale, 1).status ==
           AssuranceStoreStatus::RevisionConflict);
    auto report = sample_report(value, revised);
    auto terminal = revised;
    terminal.revision = 3;
    terminal.state = AssuranceWorkflowState::Completed;
    terminal.next_stage = AssuranceStage::Complete;
    terminal.acceptance_report_digest = encode(report).at("canonical_digest").get<std::string>();
    assert(memory.commit_report(terminal, 2, report));
    const auto loaded_memory_report = memory.load_report("tenant-a", "workflow-store");
    assert(loaded_memory_report && loaded_memory_report->report.decision == AcceptanceDecision::Accepted);

    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto path = (std::filesystem::temp_directory_path() /
                       ("taskflow-phase4-f4v-store-" + suffix + ".sqlite")).string();
    {
        SQLiteAssuranceStore store(path);
        assert(store.create_checkpoint(checkpoint));
        assert(store.compare_exchange_checkpoint(revised, 1));
        assert(store.commit_report(terminal, 2, report));
    }
    {
        SQLiteAssuranceStore reopened(path);
        const auto loaded = reopened.load_checkpoint("tenant-a", "workflow-store");
        assert(loaded && loaded->checkpoint.state == AssuranceWorkflowState::Completed);
        const auto loaded_report = reopened.load_report("tenant-a", "workflow-store");
        assert(loaded_report && loaded_report->revision == 3);
#ifndef _WIN32
        const auto permissions = std::filesystem::status(path).permissions();
        assert((permissions & std::filesystem::perms::group_read) == std::filesystem::perms::none);
        assert((permissions & std::filesystem::perms::others_read) == std::filesystem::perms::none);
#endif
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
    return 0;
}
