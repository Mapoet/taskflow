#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "agent/contracts/contract.hpp"

namespace agent_framework::tool_runtime
{

    enum class InvocationState
    {
        Created,
        Admitted,
        Queued,
        Leased,
        Running,
        Progressing,
        Checkpointed,
        AwaitingInput,
        AwaitingApproval,
        Cancelling,
        Retrying,
        Reconciling,
        CompletedCandidate,
        EffectCommitted,
        Verified,
        Failed,
        Cancelled,
        Orphaned,
        ManualReview
    };
    enum class InvocationEventDurability
    {
        Ephemeral,
        Durable
    };
    enum class InvocationStoreStatus
    {
        Committed,
        AlreadyExists,
        NotFound,
        RevisionConflict,
        FencingRejected,
        Invalid,
        Busy,
        Error
    };

    struct InvocationBudget
    {
        std::uint64_t wall_time_ms{0}, progress_events{0}, output_bytes{0};
    };
    struct InvocationLease
    {
        std::string owner, instance_id;
        std::uint64_t worker_generation{0}, fencing_token{0};
        std::int64_t expires_at_ms{0};
    };
    struct PartialResultRef
    {
        std::uint64_t sequence{0};
        std::string kind, uri, digest, media_type;
        std::uint64_t size{0};
        bool information_gain{false};
    };
    struct ProgressCheckpoint
    {
        std::uint64_t sequence{0};
        double fraction{0.0};
        std::string message, checkpoint_ref, checkpoint_digest, updated_at;
        bool information_gain{false};
    };
    struct InvocationReceipt
    {
        std::string result_digest, effect_receipt_digest, error_code, finished_at;
        bool effect_known{true};
    };

    struct LongRunningToolInvocation
    {
        contracts::ContractMetadata metadata;
        std::string invocation_id, conversation_id, turn_id, tool_call_id, tool_name;
        std::string tool_contract_revision, deployment_revision, tool_generation, input_digest;
        std::uint64_t revision{1}, attempt{0};
        InvocationState state{InvocationState::Created};
        InvocationBudget budget;
        InvocationLease lease;
        std::uint64_t progress_sequence{0};
        std::string checkpoint_ref, checkpoint_digest, created_at, updated_at;
        std::string adapter_id, adapter_revision, adapter_generation, external_operation_id;
        std::string provider_session_id, remote_task_id, adapter_peer_id;
        std::string adapter_restart_policy;
        bool idempotent{false};
    };

    struct InvocationEvent
    {
        std::string invocation_id, event_type;
        std::uint64_t sequence{0}, invocation_revision{0}, fencing_token{0};
        InvocationEventDurability durability{InvocationEventDurability::Durable};
        nlohmann::json payload = nlohmann::json::object();
        std::string payload_digest, previous_digest, event_digest, created_at;
        bool information_gain{false};
    };

    std::string_view name(InvocationState);
    std::optional<InvocationState> invocation_state(std::string_view);
    std::string_view name(InvocationEventDurability);
    nlohmann::json encode(const InvocationBudget &);
    nlohmann::json encode(const InvocationLease &);
    nlohmann::json encode(const PartialResultRef &);
    nlohmann::json encode(const ProgressCheckpoint &);
    nlohmann::json encode(const InvocationReceipt &);
    nlohmann::json encode(const LongRunningToolInvocation &);
    std::optional<LongRunningToolInvocation> decode_invocation(const nlohmann::json &,
                                                               const contracts::ParseContext & = {}, std::vector<contracts::ContractIssue> * = nullptr);
    std::vector<contracts::ContractIssue> validate(const LongRunningToolInvocation &);
    bool terminal(InvocationState) noexcept;
}
