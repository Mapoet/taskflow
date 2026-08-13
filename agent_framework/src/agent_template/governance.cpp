#include "agent/agent_template/governance.hpp"

#include <stdexcept>

namespace agent_framework::agent_template {

std::string to_string(ReplanTriggerKind value) {
    switch (value) {
        case ReplanTriggerKind::SkillUnavailable: return "skill_unavailable";
        case ReplanTriggerKind::PermissionDenied: return "permission_denied";
        case ReplanTriggerKind::ApprovalRejected: return "approval_rejected";
        case ReplanTriggerKind::ToolFailed: return "tool_failed";
        case ReplanTriggerKind::EvidenceInsufficient: return "evidence_insufficient";
        case ReplanTriggerKind::VerificationFailed: return "verification_failed";
        case ReplanTriggerKind::BudgetThresholdReached: return "budget_threshold_reached";
        case ReplanTriggerKind::UserChangedRequirement: return "user_changed_requirement";
        case ReplanTriggerKind::StagnationDetected: return "stagnation_detected";
    }
    return "unknown";
}

ReplanResult ReplanCoordinator::replan(SkillPlanProvider& provider,
                                       const PlanningContext& context,
                                       const AgentTemplate& agent_template,
                                       const SkillCollaborationPlan& current,
                                       const ReplanTrigger& trigger,
                                       const std::vector<SkillCandidate>& candidates) const {
    ReplanResult out;
    if (trigger.expected_current_revision != current.revision) {
        out.error_code = "replan_revision_conflict";
        return out;
    }
    nlohmann::json observations = trigger.observations;
    observations["trigger"] = to_string(trigger.kind);
    observations["source_node_id"] = trigger.source_node_id;
    observations["reason"] = trigger.reason;
    SkillCollaborationPlan candidate;
    try { candidate = provider.revise(context, current, observations, candidates); }
    catch (const std::exception&) {
        out.error_code = "replan_provider_failed";
        return out;
    }
    SkillCollaborationPlanValidator validator;
    auto validation = validator.validate(agent_template, candidate, context.permissions,
                                         context.budget, &current, current.revision + 1);
    if (!validation.ok) {
        out.error_code = "replan_rejected";
        out.issues = std::move(validation.issues);
        return out;
    }
    out.ok = true;
    out.plan = std::move(candidate);
    return out;
}

AgentCompletionDecision CallbackCompletionAuthority::evaluate(const CompletionEvidence& evidence) {
    if (!callback_) return {false, "", "completion_callback_missing", "", {}, {}};
    auto decision = callback_(evidence);
    if (decision.authority.empty()) {
        decision.accepted = false;
        decision.reason_code = "completion_authority_missing";
    }
    return decision;
}

TaskClosureCompletionAuthority::TaskClosureCompletionAuthority(
    harness::TaskClosureContract contract, FactsAssembler assembler,
    harness::TaskClosureController controller)
    : contract_(std::move(contract)), assembler_(std::move(assembler)),
      controller_(std::move(controller)) {}

AgentCompletionDecision TaskClosureCompletionAuthority::evaluate(const CompletionEvidence& evidence) {
    if (!assembler_) return {false, "task_closure_controller", "facts_assembler_missing", "", {}, {}};
    const auto decision = controller_.evaluate(contract_, assembler_(evidence));
    AgentCompletionDecision out;
    out.accepted = decision.state == harness::TaskTerminalState::CompletedVerified ||
                   decision.state == harness::TaskTerminalState::CompletedWithLimitations;
    out.authority = decision.terminal_authority;
    out.reason_code = decision.reason_code;
    out.decision_digest = decision.receipt_digest;
    out.evidence_refs = decision.evidence_refs;
    out.limitations = decision.limitations;
    return out;
}

}  // namespace agent_framework::agent_template
