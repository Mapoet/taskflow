#include "agent/run/types.hpp"

#include <set>

namespace agent_framework::run {
namespace {
using json = nlohmann::json;
using contracts::ContractIssue;
std::string state_string(RunState value) {
    static const char* names[] = {"created", "received", "planning", "awaiting_approval", "running",
        "waiting", "interrupted", "verifying", "replanning", "completed", "partial", "rejected",
        "failed", "cancelled"};
    return names[static_cast<std::size_t>(value)];
}
RunState state_value(const std::string& value) {
    static const std::vector<std::string> names = {"created", "received", "planning", "awaiting_approval",
        "running", "waiting", "interrupted", "verifying", "replanning", "completed", "partial",
        "rejected", "failed", "cancelled"};
    for(std::size_t i = 0; i < names.size(); ++i)
        if(names[i] == value) return static_cast<RunState>(i);
    throw std::invalid_argument("unknown run state");
}
template <typename T, typename Builder>
std::optional<T> decode_value(const json& value, const char* kind,
                              const std::set<std::string>& fields,
                              const contracts::ParseContext& context,
                              std::vector<ContractIssue>* issues, Builder builder) {
    auto document = contracts::parse_typed_contract(value, kind, context, issues);
    if(!document || !contracts::validate_object_fields(document->payload, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, issues, "/payload")) return std::nullopt;
    try {
        T result = builder(document->payload);
        result.metadata = std::move(document->metadata);
        return result;
    } catch(const std::exception& error) {
        contracts::append_issue(issues, "payload_decode_failed", "/payload", error.what());
        return std::nullopt;
    }
}
}  // namespace
json encode(const RunCheckpoint& value) {
    return contracts::make_typed_contract(value.metadata, "agent.run_checkpoint/v1",
        {{"revision", value.revision}, {"state", state_string(value.state)},
         {"graph_revision", value.graph_revision}, {"node_id", value.node_id},
         {"attempt", value.attempt}, {"pending_successors", value.pending_successors},
         {"input_digest", value.input_digest}, {"output_digest", value.output_digest},
         {"plan_digest", value.plan_digest},
         {"acceptance_contract_digest", value.acceptance_contract_digest},
         {"effect_journal_position", value.effect_journal_position},
         {"memory_snapshot_id", value.memory_snapshot_id},
         {"memory_view_digest", value.memory_view_digest}, {"created_at", value.created_at}});
}
json encode(const Interruption& value) {
    return contracts::make_typed_contract(value.metadata, "agent.interruption/v1",
        {{"interruption_id", value.interruption_id}, {"interruption_kind", value.kind},
         {"payload", value.payload}, {"policy_revision", value.policy_revision},
         {"resume_token_digest", value.resume_token_digest}, {"state_digest", value.state_digest},
         {"expires_at", value.expires_at}});
}
std::optional<RunCheckpoint> decode_run_checkpoint(
    const json& value, const contracts::ParseContext& context,
    std::vector<ContractIssue>* issues) {
    static const std::set<std::string> fields = {"revision", "state", "graph_revision", "node_id",
        "attempt", "pending_successors", "input_digest", "output_digest", "plan_digest",
        "acceptance_contract_digest", "effect_journal_position", "memory_snapshot_id",
        "memory_view_digest", "created_at"};
    return decode_value<RunCheckpoint>(value, "agent.run_checkpoint/v1", fields, context, issues,
        [](const json& p) {
            RunCheckpoint r;
            r.revision = p.at("revision").get<std::uint64_t>();
            r.state = state_value(p.at("state").get<std::string>());
            r.graph_revision = p.at("graph_revision").get<std::string>();
            r.node_id = p.at("node_id").get<std::string>();
            r.attempt = p.at("attempt").get<std::uint64_t>();
            r.pending_successors = p.at("pending_successors").get<std::vector<std::string>>();
            r.input_digest = p.at("input_digest").get<std::string>();
            r.output_digest = p.at("output_digest").get<std::string>();
            r.plan_digest = p.at("plan_digest").get<std::string>();
            r.acceptance_contract_digest = p.at("acceptance_contract_digest").get<std::string>();
            r.effect_journal_position = p.at("effect_journal_position").get<std::string>();
            r.memory_snapshot_id = p.at("memory_snapshot_id").get<std::string>();
            r.memory_view_digest = p.at("memory_view_digest").get<std::string>();
            r.created_at = p.at("created_at").get<std::string>();
            return r;
        });
}
std::optional<Interruption> decode_interruption(
    const json& value, const contracts::ParseContext& context,
    std::vector<ContractIssue>* issues) {
    static const std::set<std::string> fields = {"interruption_id", "interruption_kind", "payload",
        "policy_revision", "resume_token_digest", "state_digest", "expires_at"};
    return decode_value<Interruption>(value, "agent.interruption/v1", fields, context, issues,
        [](const json& p) {
            Interruption r;
            r.interruption_id = p.at("interruption_id").get<std::string>();
            r.kind = p.at("interruption_kind").get<std::string>();
            r.payload = p.at("payload");
            r.policy_revision = p.at("policy_revision").get<std::string>();
            r.resume_token_digest = p.at("resume_token_digest").get<std::string>();
            r.state_digest = p.at("state_digest").get<std::string>();
            r.expires_at = p.at("expires_at").get<std::string>();
            return r;
        });
}
}  // namespace agent_framework::run
