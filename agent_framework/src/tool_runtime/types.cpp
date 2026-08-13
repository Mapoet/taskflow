#include "agent/tool_runtime/types.hpp"
#include <set>
namespace agent_framework::tool_runtime
{
    namespace
    {
        using json = nlohmann::json;
        constexpr std::string_view names[] = {"created", "admitted", "queued", "leased", "running", "progressing", "checkpointed", "awaiting_input", "awaiting_approval", "cancelling", "retrying", "reconciling", "completed_candidate", "effect_committed", "verified", "failed", "cancelled", "orphaned", "manual_review"};
    }
    std::string_view name(InvocationState v) { return names[static_cast<std::size_t>(v)]; }
    std::optional<InvocationState> invocation_state(std::string_view v)
    {
        for (std::size_t i = 0; i < std::size(names); ++i)
            if (names[i] == v)
                return static_cast<InvocationState>(i);
        return std::nullopt;
    }
    std::string_view name(InvocationEventDurability v) { return v == InvocationEventDurability::Durable ? "durable" : "ephemeral"; }
    json encode(const InvocationBudget &v) { return {{"wall_time_ms", v.wall_time_ms}, {"progress_events", v.progress_events}, {"output_bytes", v.output_bytes}}; }
    json encode(const InvocationLease &v) { return {{"owner", v.owner}, {"instance_id", v.instance_id}, {"worker_generation", v.worker_generation}, {"fencing_token", v.fencing_token}, {"expires_at_ms", v.expires_at_ms}}; }
    json encode(const PartialResultRef &v) { return {{"sequence", v.sequence}, {"kind", v.kind}, {"uri", v.uri}, {"digest", v.digest}, {"media_type", v.media_type}, {"size", v.size}, {"information_gain", v.information_gain}}; }
    json encode(const ProgressCheckpoint &v) { return {{"sequence", v.sequence}, {"fraction", v.fraction}, {"message", v.message}, {"checkpoint_ref", v.checkpoint_ref}, {"checkpoint_digest", v.checkpoint_digest}, {"updated_at", v.updated_at}, {"information_gain", v.information_gain}}; }
    json encode(const InvocationReceipt &v) { return {{"result_digest", v.result_digest}, {"effect_receipt_digest", v.effect_receipt_digest}, {"error_code", v.error_code}, {"finished_at", v.finished_at}, {"effect_known", v.effect_known}}; }
    json encode(const LongRunningToolInvocation &v) { return contracts::make_typed_contract(v.metadata, "agent.long_running_tool_invocation/v1", {{"invocation_id", v.invocation_id}, {"conversation_id", v.conversation_id}, {"turn_id", v.turn_id}, {"tool_call_id", v.tool_call_id}, {"tool_name", v.tool_name}, {"tool_contract_revision", v.tool_contract_revision}, {"deployment_revision", v.deployment_revision}, {"tool_generation", v.tool_generation}, {"input_digest", v.input_digest}, {"revision", v.revision}, {"attempt", v.attempt}, {"state", name(v.state)}, {"budget", encode(v.budget)}, {"lease", encode(v.lease)}, {"progress_sequence", v.progress_sequence}, {"checkpoint_ref", v.checkpoint_ref}, {"checkpoint_digest", v.checkpoint_digest}, {"created_at", v.created_at}, {"updated_at", v.updated_at}, {"adapter_id",v.adapter_id},{"adapter_revision",v.adapter_revision},{"adapter_generation",v.adapter_generation},{"external_operation_id",v.external_operation_id},{"adapter_restart_policy",v.adapter_restart_policy},{"idempotent", v.idempotent}}); }
    std::vector<contracts::ContractIssue> validate(const LongRunningToolInvocation &v)
    {
        std::vector<contracts::ContractIssue> x;
        contracts::validate_metadata(v.metadata, &x);
        if (v.invocation_id.empty() || v.tool_call_id.empty() || v.tool_name.empty() || v.input_digest.empty())
            contracts::append_issue(&x, "identity_required", "/payload", "invocation/tool/input identity required");
        if (v.revision == 0)
            contracts::append_issue(&x, "revision_invalid", "/payload/revision", "revision must be positive");
        if ((v.state == InvocationState::Leased || v.state == InvocationState::Running || v.state == InvocationState::Progressing || v.state == InvocationState::Checkpointed) && (v.lease.owner.empty() || v.lease.fencing_token == 0 || v.lease.worker_generation == 0))
            contracts::append_issue(&x, "lease_required", "/payload/lease", "active state requires fenced lease");
        return x;
    }
    std::optional<LongRunningToolInvocation> decode_invocation(const json &j, const contracts::ParseContext &c, std::vector<contracts::ContractIssue> *issues)
    {
        static const std::set<std::string> required = {"invocation_id", "conversation_id", "turn_id", "tool_call_id", "tool_name", "tool_contract_revision", "deployment_revision", "tool_generation", "input_digest", "revision", "attempt", "state", "budget", "lease", "progress_sequence", "checkpoint_ref", "checkpoint_digest", "created_at", "updated_at", "idempotent"};
        static auto fields=[] {auto v=required;v.insert({"adapter_id","adapter_revision","adapter_generation","external_operation_id","adapter_restart_policy"});return v;}();
        auto d = contracts::parse_typed_contract(j, "agent.long_running_tool_invocation/v1", c, issues);
        if (!d || !contracts::validate_object_fields(d->payload, required, fields, c.unknown_fields, nullptr, issues, "/payload"))
            return {};
        try
        {
            const auto &p = d->payload;
            LongRunningToolInvocation v;
            v.metadata = d->metadata;
            v.invocation_id = p.at("invocation_id");
            v.conversation_id = p.at("conversation_id");
            v.turn_id = p.at("turn_id");
            v.tool_call_id = p.at("tool_call_id");
            v.tool_name = p.at("tool_name");
            v.tool_contract_revision = p.at("tool_contract_revision");
            v.deployment_revision = p.at("deployment_revision");
            v.tool_generation = p.at("tool_generation");
            v.input_digest = p.at("input_digest");
            v.revision = p.at("revision");
            v.attempt = p.at("attempt");
            auto s = invocation_state(p.at("state").get<std::string>());
            if (!s)
                throw std::runtime_error("unknown invocation state");
            v.state = *s;
            auto b = p.at("budget");
            v.budget = {b.at("wall_time_ms"), b.at("progress_events"), b.at("output_bytes")};
            auto l = p.at("lease");
            v.lease = {l.at("owner"), l.at("instance_id"), l.at("worker_generation"), l.at("fencing_token"), l.at("expires_at_ms")};
            v.progress_sequence = p.at("progress_sequence");
            v.checkpoint_ref = p.at("checkpoint_ref");
            v.checkpoint_digest = p.at("checkpoint_digest");
            v.created_at = p.at("created_at");
            v.updated_at = p.at("updated_at");
            v.adapter_id=p.value("adapter_id","");v.adapter_revision=p.value("adapter_revision","");v.adapter_generation=p.value("adapter_generation","");v.external_operation_id=p.value("external_operation_id","");v.adapter_restart_policy=p.value("adapter_restart_policy","");
            v.idempotent = p.at("idempotent");
            auto x = validate(v);
            if (!x.empty())
            {
                if (issues)
                    issues->insert(issues->end(), x.begin(), x.end());
                return {};
            }
            return v;
        }
        catch (const std::exception &e)
        {
            contracts::append_issue(issues, "payload_decode_failed", "/payload", e.what());
            return {};
        }
    }
    bool terminal(InvocationState s) noexcept { return s == InvocationState::Verified || s == InvocationState::Failed || s == InvocationState::Cancelled || s == InvocationState::ManualReview; }
}
