#include <cassert>
#include <filesystem>
#include <memory>

#include "agent/approval/store.hpp"
#include "agent/internal/platform_io.hpp"
#include "agent/memory_v2/store.hpp"
#include "agent/ui/store_backed_operations.hpp"
#include "phase4_harness_test_support.hpp"

int main() {
    using namespace agent_framework;
    using namespace phase4_harness_test;
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("phase4-store-operations-" + std::to_string(internal::current_process_id()));
    std::error_code ec; fs::remove_all(root, ec); fs::create_directories(root);
    harness::SQLiteHarnessStore harnesses((root / "harness.sqlite3").string());
    auto counters = std::make_shared<PortCounters>();
    harness::Phase4HarnessRuntime runtime(harnesses, ports(counters, false));
    const auto completed = runtime.run(start("operations-harness"));
    assert(completed.state == harness::HarnessState::Completed);

    approval::SQLiteApprovalStore approvals((root / "approval.sqlite3").string());
    approval::ApprovalRequest approval;
    approval.metadata = metadata(); approval.approval_id = "approval-operations";
    approval.request_kind = "release"; approval.requester_id = "release-agent";
    approval.scope = "release:run-harness"; approval.reason = "raw secret must not be projected";
    approval.risk_level = "medium"; approval.policy_revision = "phase4-policy-v1";
    approval.plan_digest = completed.checkpoint.pins.plan_digest;
    approval.arguments_digest = "sha256:args"; approval.artifact_digest = "sha256:artifact";
    approval.memory_view_digest = completed.checkpoint.pins.memory_view_digest;
    approval.created_at = "2026-08-11T00:00:00Z"; approval.expires_at = "2026-08-12T00:00:00Z";
    approval.proposed_change = {{"credential", "must-never-appear"}};
    assert(approvals.put_request(approval));

    auto memories = std::make_shared<memory_v2::SQLiteMemoryStore>((root / "memory.sqlite3").string());
    memory_v2::MemoryRecord memory;
    memory.metadata = metadata(); memory.metadata.identity.memory_id = "memory-operations";
    memory.record_id = "memory-operations"; memory.scope.tenant_id = "tenant-a";
    memory.scope.project_id = "project-a"; memory.scope.task_id = "task-a";
    memory.scope.level = memory_v2::MemoryLevel::Task; memory.kind = memory_v2::MemoryKind::Evidentiary;
    memory.authority = memory_v2::Authority::Verified; memory.status = memory_v2::MemoryStatus::Verified;
    memory.source_kind = "assurance-report"; memory.source_digest = "sha256:evidence";
    memory.content_type = "application/json"; memory.content = {{"secret", "must-never-appear"}};
    assert(memories->append(memory));

    SQLiteOperationsSnapshotStore snapshots((root / "operations.sqlite3").string());
    StoreBackedOperationsAssembler assembler(harnesses, approvals, *memories, snapshots);
    OperationsAssemblyRequest request;
    request.tenant_id = "tenant-a"; request.harness_id = "operations-harness";
    request.principal_id = "principal-a"; request.now = "2026-08-11T02:00:00Z";
    request.memory_subject = memory.scope;
    auto snapshot = assembler.assemble(request); assert(snapshot);
    assert(snapshot->source_revisions.size() == 3);
    assert(snapshot->hitl.size() == 1 && snapshot->hitl[0].id == approval.approval_id);
    assert(snapshot->memory.size() == 1 && snapshot->memory[0].id == memory.record_id);
    const auto display = Phase4OperationsProjection::to_json(*snapshot).dump();
    assert(display.find("must-never-appear") == std::string::npos);
    auto replay = snapshots.load(snapshot->snapshot_id); assert(replay);
    assert(Phase4OperationsProjection::to_json(*replay) == Phase4OperationsProjection::to_json(*snapshot));
    auto latest = snapshots.latest("tenant-a", "run-harness"); assert(latest);
    assert(latest->snapshot_id == snapshot->snapshot_id);
    fs::remove_all(root, ec);
    return 0;
}
