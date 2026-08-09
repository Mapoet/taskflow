#include <cassert>

#include "agent/approval/policy.hpp"

int main() {
    using namespace agent_framework::approval;
    PolicyRules rules;
    rules.denied_actions.insert("delete_tenant");
    PolicyDecisionPoint policy(rules);

    PolicyContext context;
    context.identity.tenant_id = "tenant-a";
    context.actor_id = "user-a";
    context.action = "read_repository";
    context.effect_class = "read_only";
    context.risk_level = "low";
    assert(policy.evaluate(context).outcome == PolicyOutcome::Allow);

    context.action = "delete_tenant";
    assert(policy.evaluate(context).outcome == PolicyOutcome::Deny);

    context.action = "run_tool";
    context.effect_class = "external_side_effect";
    assert(policy.evaluate(context).outcome == PolicyOutcome::RequireApproval);

    context.effect_class = "read_only";
    context.authoritative_memory_write = true;
    assert(policy.evaluate(context).outcome == PolicyOutcome::RequireApproval);

    context.authoritative_memory_write = false;
    context.changes_mandatory_criterion = true;
    assert(policy.evaluate(context).outcome == PolicyOutcome::Deny);
    context.actor_roles.insert("approver");
    assert(policy.evaluate(context).outcome == PolicyOutcome::Allow);

    context.changes_mandatory_criterion = false;
    context.two_person_required = true;
    context.requester_id = "user-a";
    assert(policy.evaluate(context).outcome == PolicyOutcome::Deny);
    context.actor_id = "reviewer-b";
    assert(policy.evaluate(context).outcome == PolicyOutcome::RequireApproval);
    return 0;
}
