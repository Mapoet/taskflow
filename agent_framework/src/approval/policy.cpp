#include "agent/approval/policy.hpp"

#include <utility>

namespace agent_framework::approval {

PolicyDecisionPoint::PolicyDecisionPoint(PolicyRules rules) : rules_(std::move(rules)) {
    if(rules_.revision.empty()) rules_.revision = "phase4-policy-v1";
}

PolicyEvaluation PolicyDecisionPoint::evaluate(const PolicyContext& context) const {
    PolicyEvaluation result;
    result.policy_revision = rules_.revision;
    const bool privileged = context.actor_roles.count("admin") != 0 ||
                            context.actor_roles.count("approver") != 0;
    if(context.identity.tenant_id.empty() || context.actor_id.empty() || context.action.empty()) {
        result.reasons.push_back("missing_identity_or_action");
        return result;
    }
    if(rules_.denied_actions.count(context.action) != 0) {
        result.reasons.push_back("action_denied_by_policy");
        return result;
    }
    if(context.two_person_required && context.actor_id == context.requester_id) {
        result.reasons.push_back("separation_of_duties_violation");
        return result;
    }
    if((context.changes_mandatory_criterion || context.lowers_acceptance_threshold) && !privileged) {
        result.reasons.push_back("acceptance_weakening_forbidden");
        return result;
    }
    const bool sensitive_memory = context.authoritative_memory_write ||
                                  context.cross_scope_memory_promotion;
    const bool sensitive_capability =
        (rules_.require_approval_for_credentials && context.credentials_requested) ||
        (rules_.require_approval_for_network_expansion && context.network_expansion_requested);
    const bool automatic_effect = rules_.automatic_effects.count(context.effect_class) != 0;
    const bool elevated_risk = context.risk_level == "high" || context.risk_level == "critical";
    if(sensitive_memory || sensitive_capability || elevated_risk || !automatic_effect ||
       context.two_person_required) {
        result.outcome = PolicyOutcome::RequireApproval;
        if(sensitive_memory) result.reasons.push_back("governed_memory_change");
        if(sensitive_capability) result.reasons.push_back("sensitive_capability_change");
        if(elevated_risk) result.reasons.push_back("elevated_risk");
        if(!automatic_effect) result.reasons.push_back("non_automatic_effect");
        if(context.two_person_required) result.reasons.push_back("two_person_review_required");
        return result;
    }
    result.outcome = PolicyOutcome::Allow;
    result.reasons.push_back("deterministic_low_risk_rule");
    return result;
}

}  // namespace agent_framework::approval
