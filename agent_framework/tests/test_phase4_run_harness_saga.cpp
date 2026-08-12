#include <cassert>
#include <filesystem>

#include "agent/contracts/contract.hpp"
#include "agent/harness/run_binding_saga.hpp"
#include "agent/internal/platform_io.hpp"
#include "phase4_harness_test_support.hpp"

int main() {
    using namespace agent_framework;
    using namespace agent_framework::harness;
    const auto root = std::filesystem::temp_directory_path() /
        ("phase4-run-harness-saga-" + std::to_string(internal::current_process_id()));
    std::error_code ec; std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    run::SQLiteRunStore runs((root / "run.sqlite3").string());
    SQLiteHarnessStore harnesses((root / "harness.sqlite3").string());

    run::RunCheckpoint run_checkpoint;
    run_checkpoint.metadata = phase4_harness_test::metadata("run-harness");
    run_checkpoint.state = run::RunState::Received;
    assert(runs.create(run_checkpoint));

    HarnessCheckpoint harness_checkpoint;
    harness_checkpoint.metadata = phase4_harness_test::metadata("run-harness");
    harness_checkpoint.harness_id = "harness-a";
    HarnessEvent event{"harness-a", 1, 1, "created", {{"ok", true}}, {}, "now"};
    const auto harness_commit = harnesses.create(harness_checkpoint, event);
    assert(harness_commit);
    const auto stored_run = runs.load("run-harness");
    assert(stored_run);

    RunHarnessBinding binding;
    binding.binding_id = "binding-a";
    binding.tenant_id = "tenant-a";
    binding.run_id = "run-harness";
    binding.harness_id = "harness-a";
    binding.stage = HarnessStage::Intake;
    binding.run_revision = stored_run->revision;
    binding.run_digest = contracts::canonical_digest(run::encode(stored_run->checkpoint)).value();
    binding.harness_revision = harness_commit.revision;
    binding.harness_digest = harness_commit.digest;
    {
        SQLiteRunHarnessSaga saga((root / "saga.sqlite3").string(), runs, harnesses);
        assert(saga.prepare(binding).committed);
        assert(saga.commit(binding.binding_id).state == RunBindingState::Committed);
        assert(saga.list_unresolved(10).size() == 1);
    }
    {
        SQLiteRunHarnessSaga reopened((root / "saga.sqlite3").string(), runs, harnesses);
        assert(reopened.reconcile(binding.binding_id).state == RunBindingState::Reconciled);
        assert(reopened.list_unresolved(10).empty());

        binding.binding_id = "binding-drift";
        binding.run_digest = "sha256:wrong";
        assert(reopened.prepare(binding).committed);
        const auto drift = reopened.reconcile(binding.binding_id);
        assert(drift.state == RunBindingState::ManualReview);
        assert(reopened.list_unresolved(10).size() == 1);
    }
    std::filesystem::remove_all(root, ec);
}
