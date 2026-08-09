#include <cassert>
#include <chrono>
#include <filesystem>

#include "phase4_remediation_test_support.hpp"

int main() {
    using namespace phase4_remediation_test;
    auto p = execution_plan("task-f5r-contracts");
    auto c = contract(p); auto r = report(p, c); auto a = assurance_checkpoint(p, c, r);
    auto i = inventory(p, r);
    const auto encoded = encode(i);
    assert(decode_impact_inventory(encoded));
    auto unknown = encoded; unknown["payload"]["unknown"] = true;
    unknown["canonical_digest"] = contracts::embedded_digest(unknown).value();
    assert(!decode_impact_inventory(unknown));
    auto tampered = encoded; tampered["payload"]["inventory_id"] = "tampered";
    assert(!decode_impact_inventory(tampered));
    auto nested_unknown = encoded; nested_unknown["payload"]["artifacts"][0]["unknown"] = true;
    nested_unknown["canonical_digest"] = contracts::embedded_digest(nested_unknown).value();
    assert(!decode_impact_inventory(nested_unknown));

    RemediationCheckpoint checkpoint; checkpoint.metadata = p.metadata;
    checkpoint.workflow_id = "store-f5r";
    checkpoint.current_plan_digest = planning::encode(p).at("canonical_digest").get<std::string>();
    checkpoint.acceptance_contract_digest = assurance::encode(c).at("canonical_digest").get<std::string>();
    checkpoint.acceptance_report_digest = assurance::encode(r).at("canonical_digest").get<std::string>();
    checkpoint.assurance_checkpoint_digest = assurance::encode(a).at("canonical_digest").get<std::string>();
    checkpoint.impact_inventory_digest = encode(i).at("canonical_digest").get<std::string>();
    checkpoint.memory_snapshot_id = "snapshot"; checkpoint.memory_view_digest = "sha256:view";
    checkpoint.updated_at = "2026-08-10T00:00:00Z";
    assert(decode_remediation_checkpoint(encode(checkpoint)));

    InMemoryRemediationStore memory;
    assert(memory.create(checkpoint));
    auto revised = checkpoint; revised.revision = 2; revised.next_stage = RemediationStage::RemediationPlanning;
    assert(memory.compare_exchange(revised, 1));
    assert(memory.compare_exchange(revised, 1).status == RemediationStoreStatus::RevisionConflict);

    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto path = (std::filesystem::temp_directory_path() /
                       ("taskflow-f5r-" + suffix + ".sqlite")).string();
    { SQLiteRemediationStore store(path); assert(store.create(checkpoint)); assert(store.compare_exchange(revised, 1)); }
    { SQLiteRemediationStore reopened(path); auto loaded = reopened.load("tenant-a", "store-f5r");
      assert(loaded && loaded->revision == 2 && loaded->checkpoint.next_stage == RemediationStage::RemediationPlanning); }
    std::filesystem::remove(path); std::filesystem::remove(path + "-wal"); std::filesystem::remove(path + "-shm");
    return 0;
}
