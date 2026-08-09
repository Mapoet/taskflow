#pragma once

#include <set>
#include <string>
#include <vector>

#include "agent/approval/types.hpp"

namespace agent_framework::approval {

enum class PolicyOutcome { Allow, Deny, RequireApproval, Modify };

struct PolicyContext {
    contracts::ContractIdentity identity;
    std::string actor_id;
    std::set<std::string> actor_roles;
    std::string action;
    std::string resource;
    std::string effect_class;
    std::string risk_level;
    std::string requester_id;
    bool changes_mandatory_criterion{false};
    bool lowers_acceptance_threshold{false};
    bool authoritative_memory_write{false};
    bool cross_scope_memory_promotion{false};
    bool two_person_required{false};
    bool credentials_requested{false};
    bool network_expansion_requested{false};
};

struct PolicyEvaluation {
    PolicyOutcome outcome{PolicyOutcome::Deny};
    std::string policy_revision;
    std::vector<std::string> reasons;
    nlohmann::json modifications = nlohmann::json::object();
};

struct PolicyRules {
    std::string revision{"phase4-policy-v1"};
    std::set<std::string> denied_actions;
    std::set<std::string> privileged_roles{"admin", "approver"};
    std::set<std::string> automatic_effects{"none", "read_only", "idempotent_local"};
    bool require_approval_for_credentials{true};
    bool require_approval_for_network_expansion{true};
};

class PolicyDecisionPoint {
public:
    explicit PolicyDecisionPoint(PolicyRules rules = {});
    PolicyEvaluation evaluate(const PolicyContext& context) const;
    const PolicyRules& rules() const noexcept { return rules_; }
private:
    PolicyRules rules_;
};

}  // namespace agent_framework::approval
