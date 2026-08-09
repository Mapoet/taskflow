#include <array>
#include <cassert>
#include <filesystem>
#include <memory>
#include <string>

#include "agent/assurance/arbiter.hpp"
#include "agent/internal/platform_io.hpp"
#include "agent/memory_v2/provider.hpp"
#include "agent/memory_v2/view_engine.hpp"
#include "agent/memory_v2/view_profiles.hpp"
#include "agent/planning/plan_store.hpp"
#include "agent/run/state_machine.hpp"
#include "agent/run/store.hpp"

namespace {
using namespace agent_framework;

contracts::ContractMetadata metadata() {
    contracts::ContractMetadata value;
    value.identity.tenant_id = "tenant-a";
    value.identity.organization_id = "org-a";
    value.identity.principal_id = "principal-a";
    value.identity.project_id = "project-a";
    value.identity.task_id = "task-a";
    value.identity.run_id = "run-a";
    value.identity.plan_id = "plan-a";
    value.extensions["policy_revision"] = "policy-v1";
    return value;
}

memory_v2::MemoryScope subject() {
    memory_v2::MemoryScope value;
    value.tenant_id = "tenant-a";
    value.organization_id = "org-a";
    value.principal_id = "principal-a";
    value.project_id = "project-a";
    value.task_id = "task-a";
    value.run_id = "run-a";
    value.level = memory_v2::MemoryLevel::Task;
    return value;
}

run::RunCheckpoint checkpoint(run::RunState state, std::string plan_digest,
                              const memory_v2::MemoryView& view) {
    run::RunCheckpoint value;
    value.metadata = metadata();
    value.state = state;
    value.graph_revision = "phase4-vertical-v1";
    value.plan_digest = std::move(plan_digest);
    value.memory_snapshot_id = view.snapshot.snapshot_id;
    value.memory_view_digest = view.manifest.view_digest;
    return value;
}
}  // namespace

int main() {
    using namespace agent_framework;
    const auto root = std::filesystem::temp_directory_path() /
        ("agent-phase4-vertical-" + std::to_string(internal::current_process_id()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    std::string plan_digest;
    std::string snapshot_id;
    std::string verification_view_digest;
    {
        auto memory_store = std::make_shared<memory_v2::SQLiteMemoryStore>(
            (root / "memory.sqlite3").string());
        memory_v2::MemoryRecord instruction;
        instruction.metadata = metadata();
        instruction.metadata.identity.memory_id = "project-instruction";
        instruction.record_id = "project-instruction";
        instruction.scope = subject();
        instruction.scope.run_id.clear();
        instruction.scope.level = memory_v2::MemoryLevel::Project;
        instruction.kind = memory_v2::MemoryKind::Instruction;
        instruction.authority = memory_v2::Authority::Authoritative;
        instruction.status = memory_v2::MemoryStatus::Authoritative;
        instruction.source_kind = "approved_project_policy";
        instruction.source_digest = "sha256:project-policy";
        instruction.content_type = "application/json";
        instruction.content = {{"constraint", "all five acceptance layers are mandatory"}};
        assert(memory_store->append(instruction, "approval-project-policy"));

        memory_v2::MemoryProviderRegistry providers;
        assert(providers.register_provider(std::make_shared<memory_v2::StoreMemoryProvider>(
            "durable-memory", memory_store)));
        memory_v2::MemoryViewEngine view_engine(providers);
        auto planning_spec = memory_v2::make_view_spec(
            memory_v2::MemoryViewMode::Planning, metadata(), subject());
        planning_spec.mandatory_record_ids = {"project-instruction"};
        const auto planning_view = view_engine.build(planning_spec, "2026-08-09T00:00:00Z");
        assert(!planning_view.fail_closed && planning_view.records.size() == 1);

        planning::ExecutionPlan plan;
        plan.metadata = metadata();
        plan.task_understanding_digest = "sha256:understanding";
        plan.evidence_bundle_digest = "sha256:evidence-bundle";
        plan.acceptance_contract_digest = "sha256:acceptance-draft";
        plan.memory_snapshot_id = planning_view.snapshot.snapshot_id;
        plan.planning_view_digest = planning_view.manifest.view_digest;
        plan.nodes.push_back({"implement", "implement the approved change", {"agent_framework"},
                              {}, {"task-intake"}, {"artifact-manifest"}, {}, {"filesystem"},
                              {"write"}, "acceptance-v1", "restore the prior artifact", "medium",
                              true});
        plan.critical_path = {"implement"};
        planning::InMemoryPlanStore plans;
        const auto planned = plans.create(plan);
        assert(planned);
        plan_digest = planned.digest;
        assert(!plan_digest.empty());

        run::SQLiteRunStore runs((root / "run.sqlite3").string());
        auto created = checkpoint(run::RunState::Created, plan_digest, planning_view);
        assert(runs.create(created));
        std::uint64_t revision = 1;
        for(const auto state : {run::RunState::Received, run::RunState::Planning,
                                run::RunState::Running, run::RunState::Verifying}) {
            const auto committed = runs.checkpoint(checkpoint(state, plan_digest, planning_view),
                                                   revision);
            assert(committed);
            revision = committed.revision;
        }

        auto verification_spec = memory_v2::make_view_spec(
            memory_v2::MemoryViewMode::Verification, metadata(), subject());
        verification_spec.mandatory_record_ids = {"project-instruction"};
        const auto verification_view = view_engine.build(
            verification_spec, "2026-08-09T00:00:00Z");
        assert(!verification_view.fail_closed);
        snapshot_id = verification_view.snapshot.snapshot_id;
        verification_view_digest = verification_view.manifest.view_digest;

        assurance::AcceptanceContract contract;
        contract.metadata = metadata();
        contract.plan_digest = plan_digest;
        const std::array<assurance::VerificationLayer, 5> layers = {
            assurance::VerificationLayer::Functional, assurance::VerificationLayer::Module,
            assurance::VerificationLayer::Integration, assurance::VerificationLayer::System,
            assurance::VerificationLayer::Metric};
        assurance::EvidenceLedger ledger;
        for(std::size_t index = 0; index < layers.size(); ++index) {
            const auto criterion_id = "criterion-" + std::to_string(index);
            contract.criteria.push_back({criterion_id, layers[index], "mandatory claim", "test",
                                         {"test"}, "pass", true});
            assurance::VerificationEvidence evidence;
            evidence.evidence_id = "evidence-" + std::to_string(index);
            evidence.criterion_id = criterion_id;
            evidence.source_kind = "test";
            evidence.source_locator = "test://phase4-vertical/" + criterion_id;
            evidence.content_digest = "sha256:evidence-" + std::to_string(index);
            evidence.observed_at = "2026-08-09T00:00:00Z";
            evidence.freshness_deadline = "2027-01-01T00:00:00Z";
            evidence.oracle_strength = assurance::OracleStrength::Deterministic;
            evidence.outcome = assurance::FindingOutcome::Pass;
            assert(ledger.append(std::move(evidence)));
        }
        assurance::ArbiterBindings bindings{metadata(), "sha256:artifact-manifest", snapshot_id,
                                             verification_view_digest,
                                             "2026-08-09T00:00:00Z"};
        const auto report = assurance::AcceptanceArbiter().decide(contract, ledger, bindings);
        assert(report.decision == assurance::AcceptanceDecision::Accepted);
        assert(report.plan_digest == plan_digest && report.findings.size() == 5);

        const auto completed = runs.checkpoint(
            checkpoint(run::RunState::Completed, plan_digest, verification_view), revision);
        assert(completed);
        assert(runs.list_recoverable(10).empty());
    }

    {
        run::SQLiteRunStore recovered((root / "run.sqlite3").string());
        const auto terminal = recovered.load("run-a");
        assert(terminal && terminal->checkpoint.state == run::RunState::Completed);
        assert(terminal->checkpoint.plan_digest == plan_digest);
        assert(terminal->checkpoint.memory_snapshot_id == snapshot_id);
        assert(terminal->checkpoint.memory_view_digest == verification_view_digest);
        assert(recovered.list_recoverable(10).empty());
    }

    std::filesystem::remove_all(root, error);
    return 0;
}
