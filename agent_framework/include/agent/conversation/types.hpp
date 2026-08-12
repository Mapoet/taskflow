#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "agent/contracts/contract.hpp"

namespace agent_framework::conversation
{

    enum class TaskExecutionProfile
    {
        Conversation,
        ReadOnlyAnalysis,
        ArtifactDelivery,
        CodeChange,
        ExternalAction,
        Professional
    };
    enum class TurnPhase
    {
        Pending,
        Running,
        AwaitingTool,
        AwaitingInput,
        Interrupted,
        Completed,
        Failed
    };
    enum class TurnContinuationReason
    {
        InitialRequest,
        ToolResultsAvailable,
        QueuedUserInput,
        ContextCompacted,
        ReplanRequested,
        ClarificationAnswered,
        ResumeAfterApproval,
        None
    };
    enum class ModelTurnStopReason
    {
        EndTurn,
        ToolRequested,
        GuardStopped,
        ProviderError,
        Cancelled,
        DeadlineExceeded,
        ContextExhausted,
        MaxIterations
    };
    enum class InputDisposition
    {
        InterruptAndReplace,
        AppendToCurrentTurn,
        QueueNextTurn,
        ControlAction,
        StatusQuery
    };
    enum class EventDurability
    {
        Ephemeral,
        Durable
    };
    enum class EventVisibility
    {
        Internal,
        User,
        Operations,
        Audit
    };
    enum class InputState
    {
        Queued,
        Consumed,
        Cancelled
    };

    struct ConversationIdentity
    {
        std::string tenant_id, conversation_id;
    };
    struct ConversationMessage
    {
        ConversationIdentity identity;
        std::string message_id, parent_id, turn_id, role, content, created_at;
        std::uint64_t sequence{0};
        std::string digest;
    };
    struct TurnRequest
    {
        ConversationIdentity identity;
        std::string turn_id, input;
        TaskExecutionProfile profile{TaskExecutionProfile::Conversation};
        std::uint64_t max_iterations{10}, max_input_tokens{0}, max_output_tokens{0};
    };
    struct ConversationInput
    {
        ConversationIdentity identity;
        std::string input_id, target_turn_id, consumed_turn_id, content, created_at;
        InputDisposition disposition{InputDisposition::AppendToCurrentTurn};
        InputState state{InputState::Queued};
        TaskExecutionProfile profile{TaskExecutionProfile::Conversation};
        std::uint64_t max_iterations{10}, max_input_tokens{0}, max_output_tokens{0};
        std::uint64_t sequence{0};
        std::string digest;
    };
    struct TurnCheckpoint
    {
        ConversationIdentity identity;
        std::string turn_id;
        std::uint64_t revision{0}, iteration{0};
        TurnPhase phase{TurnPhase::Pending};
        TurnContinuationReason continuation{TurnContinuationReason::InitialRequest};
        std::string last_message_id, compact_boundary_digest;
    };
    struct ModelTurnOutcome
    {
        ModelTurnStopReason reason{ModelTurnStopReason::EndTurn};
        std::string candidate_answer;
        std::vector<std::string> tool_receipt_refs;
        std::optional<std::string> clarification;
        bool task_completion_verified{false};
    };
    struct RuntimeEventEnvelope
    {
        std::string event_id, tenant_id, conversation_id, turn_id, run_id;
        std::optional<std::string> tool_call_id;
        std::uint64_t sequence{0};
        EventDurability durability{EventDurability::Ephemeral};
        EventVisibility visibility{EventVisibility::Internal};
        std::string event_type, timestamp, redaction_class{"public"};
        nlohmann::json payload = nlohmann::json::object();
        std::string digest;
    };
    struct QueuedTurnClaim
    {
        ConversationInput input;
        TurnCheckpoint checkpoint;
        std::vector<RuntimeEventEnvelope> durable_events;
    };
    struct ContextSegment
    {
        std::string kind, reference, digest, authority, truncation_reason;
        std::uint64_t token_budget{0};
        bool mandatory{false};
    };
    struct ContextProjectionManifest
    {
        ConversationIdentity identity;
        std::string turn_id, profile_revision_digest, prompt_revision_digest;
        std::uint64_t revision{0};
        std::vector<ContextSegment> segments;
        std::string digest;
    };
    struct CompactBoundaryRecord
    {
        ConversationIdentity identity;
        std::string boundary_id, turn_id, summary_ref, summary_digest;
        std::uint64_t revision{0}, pre_tokens{0}, post_tokens{0};
        std::vector<std::string> archived_message_ids, preserved_message_ids;
        std::string profile_revision_digest, prompt_revision_digest, model;
        std::string fallback_reason, digest;
    };

    std::string_view name(TaskExecutionProfile);
    std::string_view name(TurnPhase);
    std::string_view name(TurnContinuationReason);
    std::string_view name(ModelTurnStopReason);
    std::string_view name(InputDisposition);
    std::string_view name(InputState);
    nlohmann::json encode(const ConversationMessage &);
    nlohmann::json encode(const ConversationInput &);
    nlohmann::json encode(const TurnCheckpoint &);
    nlohmann::json encode(const RuntimeEventEnvelope &);
    std::optional<RuntimeEventEnvelope> decode_runtime_event(const nlohmann::json &,
                                                             std::string *error = nullptr);
    nlohmann::json encode(const ContextProjectionManifest &);
    nlohmann::json encode(const CompactBoundaryRecord &);
    std::optional<TaskExecutionProfile> task_execution_profile(std::string_view);
    std::vector<std::string> validate(const TurnRequest &);
    std::vector<std::string> validate(const ModelTurnOutcome &);

} // namespace agent_framework::conversation
