#include "agent/conversation/types.hpp"
#include <set>
namespace agent_framework::conversation
{
    namespace
    {
        template <class E>
        std::string_view choose(E v, std::initializer_list<std::string_view> n)
        {
            auto i = static_cast<std::size_t>(v);
            return i < n.size() ? *(n.begin() + i) : "unknown";
        }
        nlohmann::json identity(const ConversationIdentity &i) { return {{"tenant_id", i.tenant_id}, {"conversation_id", i.conversation_id}}; }
    }
    std::string_view name(TaskExecutionProfile v) { return choose(v, {"conversation", "read_only_analysis", "artifact_delivery", "code_change", "external_action", "professional"}); }
    std::string_view name(TurnPhase v) { return choose(v, {"pending", "running", "awaiting_tool", "awaiting_input", "interrupted", "completed", "failed"}); }
    std::string_view name(TurnContinuationReason v) { return choose(v, {"initial_request", "tool_results_available", "queued_user_input", "context_compacted", "replan_requested", "clarification_answered", "resume_after_approval", "none"}); }
    std::string_view name(ModelTurnStopReason v) { return choose(v, {"end_turn", "tool_requested", "guard_stopped", "provider_error", "cancelled", "deadline_exceeded", "context_exhausted", "max_iterations"}); }
    std::string_view name(InputDisposition v) { return choose(v, {"interrupt_and_replace", "append_to_current_turn", "queue_next_turn", "control_action", "status_query"}); }
    std::string_view name(InputState v) { return choose(v, {"queued", "consumed", "cancelled"}); }
    nlohmann::json encode(const ConversationMessage &v) { return {{"schema", "agent.conversation_message/v1"}, {"identity", identity(v.identity)}, {"message_id", v.message_id}, {"parent_id", v.parent_id}, {"turn_id", v.turn_id}, {"role", v.role}, {"content", v.content}, {"created_at", v.created_at}, {"sequence", v.sequence}}; }
    nlohmann::json encode(const ConversationInput &v) { return {{"schema", "agent.conversation_input/v1"}, {"identity", identity(v.identity)}, {"input_id", v.input_id}, {"target_turn_id", v.target_turn_id}, {"content", v.content}, {"created_at", v.created_at}, {"disposition", name(v.disposition)}, {"state", name(v.state)}, {"sequence", v.sequence}}; }
    nlohmann::json encode(const TurnCheckpoint &v) { return {{"schema", "agent.turn_checkpoint/v1"}, {"identity", identity(v.identity)}, {"turn_id", v.turn_id}, {"revision", v.revision}, {"iteration", v.iteration}, {"phase", name(v.phase)}, {"continuation", name(v.continuation)}, {"last_message_id", v.last_message_id}, {"compact_boundary_digest", v.compact_boundary_digest}}; }
    nlohmann::json encode(const RuntimeEventEnvelope &v) { return {{"schema", "agent.runtime_event/v1"}, {"event_id", v.event_id}, {"tenant_id", v.tenant_id}, {"conversation_id", v.conversation_id}, {"turn_id", v.turn_id}, {"run_id", v.run_id}, {"tool_call_id", v.tool_call_id ? nlohmann::json(*v.tool_call_id) : nlohmann::json(nullptr)}, {"sequence", v.sequence}, {"durability", v.durability == EventDurability::Durable ? "durable" : "ephemeral"}, {"visibility", static_cast<int>(v.visibility)}, {"event_type", v.event_type}, {"timestamp", v.timestamp}, {"redaction_class", v.redaction_class}, {"payload", v.payload}}; }
    nlohmann::json encode(const ContextProjectionManifest &v)
    {
        nlohmann::json s = nlohmann::json::array();
        for (auto &a : v.segments)
            s.push_back({{"kind", a.kind}, {"reference", a.reference}, {"digest", a.digest}, {"authority", a.authority}, {"truncation_reason", a.truncation_reason}, {"token_budget", a.token_budget}, {"mandatory", a.mandatory}});
        return {{"schema", "agent.context_projection/v1"}, {"identity", identity(v.identity)}, {"turn_id", v.turn_id}, {"revision", v.revision}, {"profile_revision_digest", v.profile_revision_digest}, {"prompt_revision_digest", v.prompt_revision_digest}, {"segments", s}};
    }
    nlohmann::json encode(const CompactBoundaryRecord &v) { return {{"schema", "agent.compact_boundary/v1"}, {"identity", identity(v.identity)}, {"boundary_id", v.boundary_id}, {"turn_id", v.turn_id}, {"revision", v.revision}, {"summary_ref", v.summary_ref}, {"summary_digest", v.summary_digest}, {"pre_tokens", v.pre_tokens}, {"post_tokens", v.post_tokens}, {"archived_message_ids", v.archived_message_ids}, {"preserved_message_ids", v.preserved_message_ids}, {"profile_revision_digest", v.profile_revision_digest}, {"prompt_revision_digest", v.prompt_revision_digest}, {"model", v.model}, {"fallback_reason", v.fallback_reason}}; }
    std::optional<TaskExecutionProfile> task_execution_profile(std::string_view v)
    {
        for (int i = 0; i < 6; ++i)
        {
            auto p = static_cast<TaskExecutionProfile>(i);
            if (name(p) == v)
                return p;
        }
        return std::nullopt;
    }
    std::vector<std::string> validate(const TurnRequest &v)
    {
        std::vector<std::string> e;
        if (v.identity.tenant_id.empty() || v.identity.conversation_id.empty() || v.turn_id.empty())
            e.push_back("turn_identity_required");
        if (v.input.empty())
            e.push_back("turn_input_required");
        if (v.max_iterations == 0)
            e.push_back("turn_iteration_bound_required");
        return e;
    }
    std::vector<std::string> validate(const ModelTurnOutcome &v)
    {
        std::vector<std::string> e;
        if (v.task_completion_verified)
            e.push_back("model_turn_cannot_verify_task");
        if (v.reason == ModelTurnStopReason::ToolRequested && v.tool_receipt_refs.empty())
            e.push_back("tool_receipt_required");
        return e;
    }
}
