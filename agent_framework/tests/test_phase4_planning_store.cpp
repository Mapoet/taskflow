#include <cassert>
#include <filesystem>
#include <string>

#include "agent/internal/platform_io.hpp"
#include "agent/planning/plan_store.hpp"

namespace {
using namespace agent_framework;

contracts::ContractMetadata scope(std::string tenant = "tenant-a") {
    contracts::ContractMetadata value;
    value.identity.tenant_id = std::move(tenant);
    value.identity.project_id = "project-a";
    value.identity.task_id = "task-a";
    value.identity.plan_id = "plan-a";
    return value;
}

planning::ExecutionPlan plan() {
    planning::ExecutionPlan value;
    value.metadata = scope();
    value.task_understanding_digest = "sha256:understanding";
    value.evidence_bundle_digest = "sha256:evidence";
    value.acceptance_contract_digest = "sha256:acceptance";
    value.memory_snapshot_id = "sha256:snapshot";
    value.planning_view_digest = "sha256:view";
    value.nodes.push_back({"node-a", "durably implement", {"agent_framework"}, {}, {"input"},
                           {"output"}, {}, {"filesystem"}, {"write"}, "criterion-a",
                           "restore previous artifact", "medium", true});
    value.critical_path = {"node-a"};
    return value;
}
}  // namespace

int main() {
    using namespace agent_framework;
    const auto root = std::filesystem::temp_directory_path() /
        ("agent-phase4-planning-" + std::to_string(internal::current_process_id()));
    const auto path = root / "planning.sqlite3";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    std::string initial_digest;
    {
        planning::SQLitePlanningStore first(path.string());
        planning::EvidenceRecord evidence{"evidence-a", "repository", "repo://CMakeLists.txt",
            "sha256:evidence-a", "2026-08-09T00:00:00Z", "direct_observation", "",
            {"claim-a"}, {}, false};
        assert(first.append(scope(), evidence));
        assert(first.append(scope(), evidence).status == planning::PlanningCommitStatus::Duplicate);
        auto duplicate_content = evidence;
        duplicate_content.evidence_id = "evidence-b";
        assert(first.append(scope(), duplicate_content).status ==
               planning::PlanningCommitStatus::Duplicate);
        auto untrusted_instruction = evidence;
        untrusted_instruction.evidence_id = "evidence-external";
        untrusted_instruction.origin_kind = "external";
        untrusted_instruction.locator = "https://example.invalid/spec";
        untrusted_instruction.content_digest = "sha256:external";
        untrusted_instruction.instruction_authority = true;
        assert(first.append(scope(), untrusted_instruction).status ==
               planning::PlanningCommitStatus::Invalid);

        auto other_tenant_evidence = evidence;
        assert(first.append(scope("tenant-b"), other_tenant_evidence));
        assert(first.get(scope(), "evidence-a"));
        assert(first.get(scope("tenant-b"), "evidence-a"));
        assert(!first.get(scope("tenant-c"), "evidence-a"));
        const auto bundle = first.bundle(scope(), {"missing", "evidence-a"});
        assert(bundle.records.size() == 1 && !bundle.bundle_id.empty());

        const auto created = first.create(plan());
        assert(created);
        initial_digest = created.digest;
        assert(first.create(plan()).status == planning::PlanningCommitStatus::Duplicate);

        planning::SQLitePlanningStore second(path.string());
        const auto recovered = second.current(scope().identity);
        assert(recovered && recovered->plan_revision == 1);
        auto revised = *recovered;
        revised.plan_revision = 2;
        revised.parent_plan_digest = initial_digest;
        revised.nodes.front().objective = "durably implement and verify";
        assert(second.compare_exchange(revised, 1));

        auto stale = revised;
        stale.plan_revision = 2;
        assert(first.compare_exchange(stale, 1).status ==
               planning::PlanningCommitStatus::RevisionConflict);
        assert(second.history(scope().identity).size() == 2);
    }

    {
        planning::SQLitePlanningStore recovered(path.string());
        const auto current = recovered.current(scope().identity);
        assert(current && current->plan_revision == 2 &&
               current->parent_plan_digest == initial_digest);
        assert(recovered.get(scope(), "evidence-a"));
    }

#if !defined(_WIN32)
    const auto permissions = std::filesystem::status(path).permissions();
    assert((permissions & std::filesystem::perms::group_all) == std::filesystem::perms::none);
    assert((permissions & std::filesystem::perms::others_all) == std::filesystem::perms::none);
#endif
    std::filesystem::remove_all(root, error);
    return 0;
}
