#include <cassert>
#include "agent/harness/task_closure.hpp"
#include "agent/internal/platform_io.hpp"
#include "phase4_harness_test_support.hpp"
#include <filesystem>

int main()
{
    using namespace agent_framework::harness;
    TaskClosureContract contract;
    contract.metadata = phase4_harness_test::metadata("closure-task");
    contract.contract_id = "closure-1";
    contract.revision = "r1";
    contract.task_class = "artifact_delivery";
    contract.deliverables = {"artifact"};
    contract.mandatory_criteria = {"file_exists", "content_valid"};
    contract.verification_methods = {{"file_exists", {"artifact"}}, {"content_valid", {"test"}}};
    contract.clarification_policy = "ask";
    assert(validate(contract).empty());
    TaskClosureController controller;
    ClosureFacts facts;
    facts.checkpoint.metadata = contract.metadata;
    facts.checkpoint.harness_id = "harness";
    facts.checkpoint.revision = 1;
    facts.checkpoint.pins.intake_digest = "sha256:intake";
    facts.checkpoint.pins.acceptance_contract_digest = "sha256:contract";
    facts.checkpoint.pins.profile_revision_digest = "sha256:profile";
    facts.checkpoint.pins.prompt_revision_digest = "sha256:prompt";
    facts.checkpoint.state = HarnessState::Running;
    facts.satisfied_criteria = {"file_exists"};
    facts.strong_evidence_refs = {"e1"};
    facts.artifact_refs = {"a1"};
    auto d = controller.evaluate(contract, facts);
    assert(d.state == TaskTerminalState::MinimalRemediation && !d.receipt_digest.empty());
    facts.missing_facts = {"target path"};
    assert(controller.evaluate(contract, facts).state == TaskTerminalState::NeedsUserInput);
    facts.missing_facts.clear();
    facts.external_blockers = {"provider unavailable"};
    assert(controller.evaluate(contract, facts).state == TaskTerminalState::BlockedExternal);
    facts.external_blockers.clear();
    facts.progress.consecutive_no_progress = 2;
    assert(controller.evaluate(contract, facts).state == TaskTerminalState::Stagnated);
    facts.progress.consecutive_no_progress = 0;
    facts.last_progress_revision = 7;
    ProgressObservation p{"p1", 1, {}, {"e1"}, {"a1"}, {}, {"f1"}, {}, "plan", 10, 5, 1, 0.1};
    ProgressObservation current = p;
    current.observation_id = "p2";
    current.revision = 2;
    auto progress = ProgressEvaluator::assess(p, current, 0);
    assert(!progress.information_gain && progress.consecutive_no_progress == 1);
    current.closed_criteria = {"file_exists"};
    progress = ProgressEvaluator::assess(p, current, 1);
    assert(progress.information_gain && progress.consecutive_no_progress == 0);
    const auto ledger_path = std::filesystem::temp_directory_path() /
        ("phase4-progress-" + std::to_string(agent_framework::internal::current_process_id()) + ".sqlite3");
    std::error_code ledger_ec; std::filesystem::remove(ledger_path, ledger_ec);
    { SQLiteProgressLedger ledger(ledger_path.string());
      assert(ledger.append("tenant-a", "task-a", current, progress));
      assert(!ledger.append("tenant-a", "task-a", current, progress));
      assert(ledger.latest("tenant-a", "task-a")->revision == 2);
      assert(ledger.latest_assessment("tenant-a", "task-a")->information_gain); }
    { SQLiteProgressLedger reopened(ledger_path.string());
      assert(reopened.latest("tenant-a", "task-a")->observation_id == "p2"); }
    std::filesystem::remove(ledger_path, ledger_ec);
    facts.checkpoint.state = HarnessState::Completed;
    facts.checkpoint.next_stage = HarnessStage::Complete;
    facts.checkpoint.pins.plan_digest = "sha256:plan";
    facts.checkpoint.pins.memory_snapshot_id = "snapshot";
    facts.checkpoint.pins.memory_view_digest = "sha256:view";
    facts.checkpoint.pins.approval_decision_id = "approval";
    facts.checkpoint.pins.artifact_manifest_digest = "sha256:artifact";
    facts.checkpoint.pins.acceptance_report_digest = "sha256:report";
    facts.checkpoint.pins.judge_report_digest = "sha256:judge";
    facts.checkpoint.pins.operations_snapshot_digest = "sha256:operations";
    for (auto stage : {HarnessStage::Intake, HarnessStage::Cognition,
                       HarnessStage::PlanApproval, HarnessStage::Execution,
                       HarnessStage::MemoryUpdate, HarnessStage::Assurance,
                       HarnessStage::Judge, HarnessStage::Operations})
        facts.checkpoint.stage_records.push_back({stage, 1, StageOutcome::Succeeded});
    facts.satisfied_criteria = contract.mandatory_criteria;
    facts.finding_refs.clear();
    auto completed = controller.evaluate(contract, facts);
    assert(completed.state == TaskTerminalState::CompletedVerified);
    assert(completed.terminal_authority == "task_closure_controller");
    assert(completed.last_progress_revision == 7);
    auto denied = ProductionTaskRouter::route({true,true,true,true,false,true,true});
    assert(!denied.allowed && denied.route == "fail_closed" && denied.missing_dependencies.size() == 1);
    assert(ProductionTaskRouter::route({false}).allowed);
    assert(ProductionTaskRouter::route({true,true,true,true,true,true,true}).allowed);
    facts.unknown_side_effect = true;
    assert(controller.evaluate(contract, facts).state == TaskTerminalState::ManualReview);
}
