#pragma once

#include <optional>
#include <string>
#include <vector>

#include "agent/approval/store.hpp"
#include "agent/assurance/professional_workflow.hpp"
#include "agent/conversation/store.hpp"
#include "agent/planning/plan_store.hpp"
#include "agent/remediation/remediation_workflow.hpp"
#include "agent/run/store.hpp"
#include "agent/session/session_store.hpp"
#include "agent/ui/interaction_source_adapters.hpp"

namespace agent_framework::ui {

enum class InteractionAssemblerError {
    None, MissingRequiredStore, MissingRequiredObject, IdentityMismatch,
    RevisionInvalid, AmbiguousObject, UnsafeDisplayData
};

struct ProductionInteractionQuery {
    contracts::ContractIdentity identity;
    std::string conversation_id;
    std::string turn_id;
    std::string message_id;
    std::string session_id;
    std::string approval_id;
    std::string assurance_workflow_id;
    std::string remediation_workflow_id;
    std::string now;
    bool require_conversation{true};
    bool require_run{true};
    bool require_plan{true};
};

struct ProductionInteractionStores {
    conversation::ConversationStore* conversations{nullptr};
    run::RunStore* runs{nullptr};
    planning::PlanStore* plans{nullptr};
    SessionStore* sessions{nullptr};
    approval::ApprovalStore* approvals{nullptr};
    assurance::AssuranceStore* assurance{nullptr};
    remediation::RemediationStore* remediation{nullptr};
};

struct ProductionInteractionResult {
    std::optional<InteractionSnapshot> snapshot;
    InteractionAssemblerError error{InteractionAssemblerError::None};
    std::vector<std::string> diagnostics;
    explicit operator bool() const noexcept { return snapshot.has_value(); }
};

// Reads canonical domain stores directly. Production assembly is fail-closed:
// required objects, identities and positive revisions must agree before any
// projection is returned.
ProductionInteractionResult assemble_production_interactions(
    const ProductionInteractionStores&, const ProductionInteractionQuery&);

} // namespace agent_framework::ui
