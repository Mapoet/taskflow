#include <cassert>
#include <filesystem>

#include "agent/harness/runtime.hpp"
#include "agent/internal/platform_io.hpp"
#include "phase4_harness_test_support.hpp"

int main() {
    using namespace agent_framework;
    using namespace agent_framework::harness;
    using namespace phase4_harness_test;

    const auto root = std::filesystem::temp_directory_path() /
        ("phase4-harness-integration-" + std::to_string(internal::current_process_id()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    auto counters = std::make_shared<PortCounters>();
    SQLiteHarnessStore store((root / "harness.sqlite3").string());
    Phase4HarnessRuntime runtime(store, ports(counters));
    HarnessRuntimeOptions options;
    options.now = [] { return "2026-08-11T00:00:00Z"; };
    const auto result = runtime.run(start(), options);
    assert(result.state == HarnessState::Completed);
    assert(result.checkpoint.remediation_cycle == 1);
    assert(result.checkpoint.pins.plan_digest == "sha256:plan-v2");
    assert(result.checkpoint.pins.artifact_manifest_digest == "sha256:artifact-v2");
    assert(result.checkpoint.pins.acceptance_report_digest ==
           "sha256:acceptance-report-v2");
    assert(result.checkpoint.unresolved_findings.empty());
    assert(Phase4HarnessRuntime::completion_gate_issues(result.checkpoint).empty());
    assert(counters->execute[HarnessStage::Execution] == 1);
    assert(counters->execute[HarnessStage::Reexecution] == 1);
    const auto events = store.events("tenant-a", "harness-a");
    assert(events.size() == result.checkpoint.revision);
    for(std::size_t index = 0; index < events.size(); ++index)
        assert(events[index].sequence == index + 1);

    const auto snapshot = Phase4HarnessRuntime::project_operations(result.checkpoint);
    const auto safe = Phase4OperationsProjection::from_json(
        Phase4OperationsProjection::to_json(snapshot));
    assert(safe.run_id == "run-harness");
    assert(safe.overall_status == OperationsStatus::Passed);
    assert(!safe.stages.empty());

    InMemoryHarnessStore approval_store;
    auto approval_counters = std::make_shared<PortCounters>();
    Phase4HarnessRuntime approval_runtime(
        approval_store, ports(approval_counters, false, true, true));
    const auto waiting = approval_runtime.run(start("harness-approval"), options);
    assert(waiting.state == HarnessState::AwaitingApproval);
    assert(waiting.checkpoint.next_stage == HarnessStage::PlanApproval);
    assert(waiting.checkpoint.pins.approval_decision_id.empty());
    const auto resumed = approval_runtime.resume("tenant-a", "harness-approval", options);
    assert(resumed.state == HarnessState::Completed);
    assert(resumed.checkpoint.pins.approval_decision_id == "approval-plan-v1");
    assert(approval_counters->execute[HarnessStage::PlanApproval] == 2);

    InMemoryHarnessStore ambiguous_store;
    auto ambiguous_counters = std::make_shared<PortCounters>();
    Phase4HarnessRuntime ambiguous_runtime(
        ambiguous_store, ports(ambiguous_counters, false, true, false, true));
    const auto ambiguous = ambiguous_runtime.run(start("harness-ambiguous-findings"), options);
    assert(ambiguous.state == HarnessState::ManualReview);
    assert(ambiguous.checkpoint.terminal_reason ==
           "accepted_report_contains_unclassified_findings");
    assert(ambiguous.checkpoint.unresolved_findings ==
           std::vector<std::string>{"unclassified-finding"});

    std::filesystem::remove_all(root, error);
    return 0;
}
