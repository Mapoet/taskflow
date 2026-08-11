#include <cassert>
#include <filesystem>

#include "agent/harness/store.hpp"
#include "agent/internal/platform_io.hpp"
#include "phase4_harness_test_support.hpp"

int main() {
    using namespace agent_framework;
    using namespace agent_framework::harness;
    using namespace phase4_harness_test;

    HarnessCheckpoint checkpoint;
    checkpoint.metadata = metadata();
    checkpoint.harness_id = "harness-contract";
    checkpoint.revision = 1;
    checkpoint.state = HarnessState::Running;
    checkpoint.next_stage = HarnessStage::Cognition;
    checkpoint.max_remediation_cycles = 2;
    checkpoint.pins.intake_digest = "sha256:intake";
    checkpoint.pins.acceptance_contract_digest = "sha256:acceptance";
    checkpoint.pins.profile_revision_digest = "sha256:profile";
    checkpoint.pins.prompt_revision_digest = "sha256:prompt";
    checkpoint.outbox.push_back({"effect-a", HarnessStage::Cognition, 1, "idem-a",
                                 "sha256:request", OutboxState::Pending, {}, {}});
    checkpoint.updated_at = "2026-08-11T00:00:00Z";

    const auto document = encode(checkpoint);
    auto decoded = decode_harness_checkpoint(document);
    assert(decoded && decoded->harness_id == checkpoint.harness_id);
    assert(decoded->outbox.size() == 1 && decoded->outbox[0].state == OutboxState::Pending);

    auto unknown = document;
    unknown["payload"]["private_chain_of_thought"] = "must be rejected";
    unknown["canonical_digest"] = *contracts::embedded_digest(unknown);
    assert(!decode_harness_checkpoint(unknown));

    auto tampered = document;
    tampered["payload"]["harness_id"] = "tampered";
    assert(!decode_harness_checkpoint(tampered));

    const auto root = std::filesystem::temp_directory_path() /
        ("phase4-harness-contract-" + std::to_string(internal::current_process_id()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    {
        SQLiteHarnessStore store((root / "harness.sqlite3").string());
        HarnessEvent created{checkpoint.harness_id, 1, 1, "created", {{"ok", true}}, {},
                             checkpoint.updated_at};
        auto committed = store.create(checkpoint, created);
        assert(committed && committed.revision == 1);
        assert(store.create(checkpoint, created).status == HarnessStoreStatus::AlreadyExists);

        auto next = checkpoint;
        next.revision = 2;
        next.next_stage = HarnessStage::PlanApproval;
        next.updated_at = "2026-08-11T00:00:01Z";
        HarnessEvent advanced{next.harness_id, 2, 2, "advanced", {{"stage", "plan_approval"}},
                              {}, next.updated_at};
        assert(store.compare_exchange(next, 1, advanced));
        assert(store.compare_exchange(next, 1, advanced).status ==
               HarnessStoreStatus::RevisionConflict);
        auto loaded = store.load("tenant-a", checkpoint.harness_id);
        assert(loaded && loaded->revision == 2 &&
               loaded->checkpoint.next_stage == HarnessStage::PlanApproval);
        const auto events = store.events("tenant-a", checkpoint.harness_id);
        assert(events.size() == 2 && events[0].sequence == 1 && events[1].sequence == 2);
        assert(store.list_recoverable("tenant-a", 10).size() == 1);
    }
    {
        SQLiteHarnessStore recovered((root / "harness.sqlite3").string());
        const auto loaded = recovered.load("tenant-a", checkpoint.harness_id);
        assert(loaded && loaded->revision == 2);
    }
    std::filesystem::remove_all(root, error);
    return 0;
}
