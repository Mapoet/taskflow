#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/contracts/contract.hpp"

namespace agent_framework::ui
{
    inline constexpr std::string_view interaction_snapshot_event_type{"interaction_snapshot"};

    enum class InteractionNodeKind
    {
        Message,
        Understanding,
        Decision,
        Thinking,
        CognitionStage,
        Plan,
        PlanNode,
        MemoryView,
        Agent,
        SkillNode,
        ToolInvocation,
        Approval,
        Evidence,
        Finding,
        Artifact,
        Closure
    };
    enum class InteractionEdgeKind
    {
        OriginatedFrom,
        PlannedBy,
        Implements,
        DelegatedTo,
        ParentOf,
        UsedMemoryView,
        RequestedApproval,
        Resumes,
        Produced,
        VerifiedBy,
        Supports,
        Contradicts,
        Supersedes,
        ClosedBy
    };
    enum class InteractionVisibility
    {
        User,
        Operations,
        Audit
    };
    enum class InteractionObjectState
    {
        Pending,
        Running,
        Waiting,
        Passed,
        Warning,
        Blocked,
        Failed,
        Superseded,
        Unavailable
    };

    std::string_view name(InteractionNodeKind) noexcept;
    std::string_view name(InteractionEdgeKind) noexcept;
    std::string_view name(InteractionVisibility) noexcept;
    std::string_view name(InteractionObjectState) noexcept;
    std::optional<InteractionNodeKind> interaction_node_kind(std::string_view) noexcept;
    std::optional<InteractionEdgeKind> interaction_edge_kind(std::string_view) noexcept;
    std::optional<InteractionVisibility> interaction_visibility(std::string_view) noexcept;
    std::optional<InteractionObjectState> interaction_object_state(std::string_view) noexcept;

    struct InteractionRef
    {
        std::string tenant_id;
        std::string conversation_id, turn_id, message_id;
        std::string task_id, run_id, harness_id;
        std::string decision_id;
        std::string plan_id, plan_node_id;
        std::uint64_t plan_revision{0};
        std::string agent_template_id, agent_invocation_id, child_agent_id;
        std::string skill_node_id, tool_invocation_id;
        std::string memory_snapshot_id, memory_view_digest;
        std::string approval_id, evidence_id, finding_id, artifact_id;
        std::string object_revision_digest;
    };

    struct InteractionSourceRevision
    {
        std::string store, object_id;
        std::uint64_t revision{0};
        std::string digest;
    };

    struct InteractionNode
    {
        std::string node_id;
        InteractionNodeKind kind{InteractionNodeKind::Message};
        InteractionRef ref;
        std::uint64_t revision{1};
        std::string label, summary;
        // View-safe, typed-by-kind presentation data. Raw prompts, credentials and
        // hidden chain-of-thought are rejected by the contract validator.
        nlohmann::json display = nlohmann::json::object();
        InteractionObjectState state{InteractionObjectState::Pending};
        InteractionVisibility visibility{InteractionVisibility::Operations};
        InteractionSourceRevision source;
        std::string updated_at, digest;
    };

    struct InteractionEdge
    {
        std::string edge_id;
        InteractionEdgeKind kind{InteractionEdgeKind::OriginatedFrom};
        std::string from_node_id, to_node_id;
        std::uint64_t revision{1};
        InteractionVisibility visibility{InteractionVisibility::Operations};
        InteractionSourceRevision source;
        std::string updated_at, digest;
    };

    struct InteractionNavigationTarget
    {
        std::string view, object_id;
        std::uint64_t revision{0};
    };

    struct UiInteractionEvent
    {
        std::string event_id, tenant_id, conversation_id;
        std::uint64_t sequence{0};
        std::string event_type;
        InteractionVisibility visibility{InteractionVisibility::Operations};
        InteractionRef primary_ref;
        std::vector<InteractionRef> related_refs;
        nlohmann::json display = nlohmann::json::object();
        InteractionNavigationTarget navigation_target;
        InteractionSourceRevision source;
        std::string timestamp, digest;
    };

    struct InteractionSnapshot
    {
        std::string tenant_id, conversation_id;
        std::uint64_t revision{0}, head_sequence{0};
        std::vector<InteractionNode> nodes;
        std::vector<InteractionEdge> edges;
        std::vector<std::string> orphan_edge_ids;
        std::vector<InteractionSourceRevision> source_revisions;
        std::string updated_at, digest;
    };

    nlohmann::json encode(const InteractionRef &);
    nlohmann::json encode(const InteractionSourceRevision &);
    nlohmann::json encode(const InteractionNode &);
    nlohmann::json encode(const InteractionEdge &);
    nlohmann::json encode(const UiInteractionEvent &);
    nlohmann::json encode(const InteractionSnapshot &);
    std::optional<InteractionRef> decode_interaction_ref(const nlohmann::json &,
                                                         std::vector<contracts::ContractIssue> * = nullptr);
    std::optional<InteractionNode> decode_interaction_node(const nlohmann::json &,
                                                           std::vector<contracts::ContractIssue> * = nullptr);
    std::optional<InteractionEdge> decode_interaction_edge(const nlohmann::json &,
                                                           std::vector<contracts::ContractIssue> * = nullptr);
    std::optional<UiInteractionEvent> decode_interaction_event(const nlohmann::json &,
                                                               std::vector<contracts::ContractIssue> * = nullptr);
    std::optional<InteractionSnapshot> decode_interaction_snapshot(
        const nlohmann::json &, std::vector<contracts::ContractIssue> * = nullptr);
    std::vector<contracts::ContractIssue> validate(const InteractionRef &,
                                                   std::optional<InteractionNodeKind> kind = std::nullopt);
    std::vector<contracts::ContractIssue> validate(const InteractionNode &);
    std::vector<contracts::ContractIssue> validate(const InteractionEdge &);
    std::vector<contracts::ContractIssue> validate(const UiInteractionEvent &);

    bool visibility_allows(InteractionVisibility viewer, InteractionVisibility object) noexcept;
    bool contains_forbidden_display_field(const nlohmann::json &) noexcept;

} // namespace agent_framework::ui
