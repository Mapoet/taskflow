#include <agent/agent_template/governance.hpp>

#include <cassert>
#include <iostream>

using namespace agent_framework;
using namespace agent_framework::agent_template;

namespace {
contracts::ContractMetadata metadata() {
    contracts::ContractMetadata value;
    value.identity.tenant_id = "tenant";
    value.identity.task_id = "task";
    return value;
}
AgentTemplate make_template() {
    AgentTemplate value;
    value.metadata = metadata();
    value.template_id = "governed";
    value.permissions.tools = {"read"};
    SkillCollaborationPlan skeleton;
    skeleton.metadata = metadata();
    skeleton.plan_id = "plan";
    SkillPlanNode required;
    required.node_id = "required";
    required.resolved_skill_id = "skill";
    required.output.evidence_required = true;
    required.verifier = true;
    skeleton.nodes = {required};
    value.workflow_skeleton = skeleton;
    return value;
}
SkillCollaborationPlan current_plan(const AgentTemplate& value) {
    auto plan = *value.workflow_skeleton;
    plan.budget = value.budgets;
    plan.committed_effect_receipts = {"effect-1"};
    return plan;
}
}  // namespace

int main() {
    const auto agent_template = make_template();
    const auto current = current_plan(agent_template);
    PlanningContext context{metadata(), agent_template, nlohmann::json::object(),
                            agent_template.permissions, agent_template.budgets, 2};
    ModelSkillPlanProvider valid_provider([](const PlanningContext&,
                                             const std::optional<SkillCollaborationPlan>& prior,
                                             const nlohmann::json& observations,
                                             const std::vector<SkillCandidate>&) {
        assert(observations.at("trigger") == "verification_failed");
        auto next = *prior;
        ++next.revision;
        next.parent_digest = encode(*prior).at("canonical_digest");
        return next;
    });
    ReplanCoordinator coordinator;
    ReplanTrigger trigger{ReplanTriggerKind::VerificationFailed, "required",
                          "oracle mismatch", nlohmann::json::object(), 1};
    auto revised = coordinator.replan(valid_provider, context, agent_template,
                                      current, trigger, {});
    assert(revised.ok && revised.plan->revision == 2);

    auto stale = trigger;
    stale.expected_current_revision = 0;
    assert(coordinator.replan(valid_provider, context, agent_template,
                              current, stale, {}).error_code == "replan_revision_conflict");

    ModelSkillPlanProvider escalation_provider([](const PlanningContext&,
        const std::optional<SkillCollaborationPlan>& prior, const nlohmann::json&,
        const std::vector<SkillCandidate>&) {
        auto next = *prior;
        ++next.revision;
        next.parent_digest = encode(*prior).at("canonical_digest");
        next.nodes.front().requested_permissions.tools = {"admin"};
        return next;
    });
    auto denied = coordinator.replan(escalation_provider, context, agent_template,
                                     current, trigger, {});
    assert(!denied.ok && denied.error_code == "replan_rejected");

    CallbackCompletionAuthority missing_authority([](const CompletionEvidence&) {
        AgentCompletionDecision decision;
        decision.accepted = true;
        return decision;
    });
    assert(!missing_authority.evaluate({}).accepted);

    harness::TaskClosureContract contract;
    contract.metadata = metadata();
    contract.contract_id = "closure";
    contract.revision = "1";
    contract.task_class = "agent-template";
    contract.mandatory_criteria = {"verified-output"};
    contract.verification_methods["verified-output"] = {"runner-receipt", "artifact"};
    TaskClosureCompletionAuthority closure(contract, [](const CompletionEvidence&) {
        harness::ClosureFacts facts;
        facts.checkpoint.metadata = metadata();
        facts.checkpoint.state = harness::HarnessState::Completed;
        facts.checkpoint.judge_required = true;
        facts.checkpoint.pins.intake_digest = "sha256:intake";
        facts.checkpoint.pins.plan_digest = "sha256:plan";
        facts.checkpoint.pins.acceptance_contract_digest = "sha256:contract";
        facts.checkpoint.pins.memory_snapshot_id = "memory-1";
        facts.checkpoint.pins.memory_view_digest = "sha256:view";
        facts.checkpoint.pins.approval_decision_id = "approval-1";
        facts.checkpoint.pins.artifact_manifest_digest = "sha256:artifact";
        facts.checkpoint.pins.acceptance_report_digest = "sha256:acceptance";
        facts.checkpoint.pins.judge_report_digest = "sha256:judge";
        facts.checkpoint.pins.operations_snapshot_digest = "sha256:operations";
        for (const auto stage : {harness::HarnessStage::Intake,
                                 harness::HarnessStage::Cognition,
                                 harness::HarnessStage::PlanApproval,
                                 harness::HarnessStage::Execution,
                                 harness::HarnessStage::MemoryUpdate,
                                 harness::HarnessStage::Assurance,
                                 harness::HarnessStage::Judge,
                                 harness::HarnessStage::Operations}) {
            harness::HarnessStageRecord record;
            record.stage = stage;
            record.outcome = harness::StageOutcome::Succeeded;
            facts.checkpoint.stage_records.push_back(record);
        }
        facts.satisfied_criteria = {"verified-output"};
        facts.strong_evidence_refs = {"evidence:runner"};
        facts.artifact_refs = {"artifact:output"};
        facts.last_progress_revision = 1;
        return facts;
    });
    const auto accepted = closure.evaluate({});
    assert(accepted.accepted);
    assert(accepted.authority == "task_closure_controller");
    std::cout << "test_agent_template_governance: ok\n";
}
