#include <cassert>
#include <memory>

#include "agent/memory_v2/view_engine.hpp"
#include "agent/planning/cognition_workflow.hpp"

namespace {
using namespace agent_framework;

class RepoInvestigator final : public planning::Investigator {
public:
    std::string id() const override { return "repo"; }
    bool external() const noexcept override { return false; }
    std::vector<planning::EvidenceRecord> investigate(
        const planning::InvestigationRequest&, std::string*) override {
        return {{"ev-repo", "repository", "/repo/CMakeLists.txt", "sha256:repo",
                 "2026-08-09T00:00:00Z", "direct_observation", "", {"claim-build"}, {}, false}};
    }
};

class OfficialDocsInvestigator final : public planning::Investigator {
public:
    std::string id() const override { return "official-docs"; }
    bool external() const noexcept override { return true; }
    std::vector<planning::EvidenceRecord> investigate(
        const planning::InvestigationRequest&, std::string*) override {
        return {{"ev-doc", "external", "https://example.invalid/spec", "sha256:doc",
                 "2026-08-09T00:00:00Z", "official", "2027-01-01T00:00:00Z",
                 {"claim-api"}, {}, true}};
    }
};

class DraftModel final : public planning::CognitionModel {
public:
    std::optional<planning::CognitionDraft> draft(
        const planning::TaskIntake& intake, const planning::EvidenceBundle& evidence,
        const memory_v2::MemoryView& view, std::string*) override {
        assert(evidence.records.size() == 2);
        assert(!evidence.records[0].instruction_authority);
        assert(!view.manifest.view_digest.empty());
        planning::CognitionDraft result;
        result.understanding.domain = "software";
        result.understanding.current_state = "legacy API";
        result.understanding.target_state = intake.user_goal;
        result.understanding.evidence_ids = {"ev-doc", "ev-repo"};
        result.understanding.change_mode = planning::ChangeMode::Upgrade;
        result.understanding.blast_radius = "public API and callers";
        result.plan.metadata.identity.plan_id = "plan-a";
        result.plan.acceptance_contract_digest = "sha256:acceptance";
        planning::PlanNode inspect;
        inspect.node_id = "inspect";
        inspect.objective = "map callers";
        inspect.output_contracts = {"caller-map"};
        inspect.acceptance_contract_id = "criterion-map";
        inspect.risk_level = "low";
        planning::PlanNode change;
        change.node_id = "change";
        change.objective = "upgrade API and consumers";
        change.dependencies = {"inspect"};
        change.output_contracts = {"patched-api"};
        change.acceptance_contract_id = "criterion-build";
        change.side_effects = {"workspace-write"};
        change.rollback_strategy = "revert patch";
        change.risk_level = "medium";
        result.plan.nodes = {inspect, change};
        result.plan.critical_path = {"inspect", "change"};
        return result;
    }
};
}

int main() {
    using namespace agent_framework;
    memory_v2::MemoryProviderRegistry providers;
    memory_v2::MemoryViewEngine views(providers);
    planning::InvestigatorRegistry investigators;
    assert(investigators.register_investigator(std::make_shared<RepoInvestigator>()));
    assert(investigators.register_investigator(std::make_shared<OfficialDocsInvestigator>()));
    planning::InMemoryEvidenceStore evidence;
    planning::InMemoryPlanStore plans;
    DraftModel model;
    planning::CognitionWorkflow workflow(views, investigators, evidence, plans, model);
    planning::TaskIntake intake;
    intake.metadata.identity.tenant_id = "tenant-a";
    intake.metadata.identity.principal_id = "user-a";
    intake.metadata.identity.project_id = "project-a";
    intake.metadata.identity.task_id = "task-a";
    intake.metadata.identity.plan_id = "plan-a";
    intake.user_goal = "upgrade the public API";
    intake.requested_deliverables = {"code", "tests"};
    memory_v2::MemoryScope subject;
    subject.tenant_id = "tenant-a";
    subject.principal_id = "user-a";
    subject.project_id = "project-a";
    subject.task_id = "task-a";
    const auto result = workflow.run(intake, subject);
    assert(result.outcome == planning::CognitionOutcome::Approved);
    assert(result.plan);
    assert(result.issues.empty());
    assert(plans.current(intake.metadata.identity)->nodes.size() == 2);

    auto revised = *result.plan;
    revised.plan_revision = 2;
    revised.parent_plan_digest = planning::encode(*result.plan).at("canonical_digest");
    revised.nodes[1].approval_required = true;
    assert(plans.compare_exchange(revised, 1));
    assert(plans.compare_exchange(revised, 1).status ==
           planning::PlanningCommitStatus::RevisionConflict);

    revised.nodes[1].dependencies = {"change"};
    const auto invalid = planning::PlanValidator{}.validate(revised);
    assert(!invalid.valid());
    return 0;
}
