#include "agent/harness/types.hpp"

#include <array>
#include <set>
#include <stdexcept>

namespace agent_framework::harness {
namespace {
using json = nlohmann::json;

template <typename Enum, std::size_t Size>
std::string enum_name(Enum value, const std::array<const char*, Size>& names) {
    const auto index = static_cast<std::size_t>(value);
    if(index >= names.size()) throw std::invalid_argument("enum value is out of range");
    return names[index];
}

template <typename Enum, std::size_t Size>
std::optional<Enum> enum_value(std::string_view value,
                               const std::array<const char*, Size>& names) {
    for(std::size_t index = 0; index < names.size(); ++index)
        if(value == names[index]) return static_cast<Enum>(index);
    return std::nullopt;
}

constexpr std::array kStages = {
    "intake", "cognition", "plan_approval", "execution", "memory_update",
    "assurance", "remediation", "reexecution", "reverification", "judge",
    "operations", "complete"};
constexpr std::array kStates = {
    "running", "awaiting_approval", "manual_review", "completed", "rejected",
    "failed", "cancelled"};
constexpr std::array kOutcomes = {
    "succeeded", "awaiting_approval", "needs_remediation", "rejected", "retryable",
    "manual_review", "failed", "cancelled"};
constexpr std::array kOutboxStates = {"pending", "committed", "rejected", "unknown"};

json encode_pins(const PinnedRevisions& value) {
    return {{"intake_digest", value.intake_digest},
            {"plan_digest", value.plan_digest},
            {"acceptance_contract_digest", value.acceptance_contract_digest},
            {"memory_snapshot_id", value.memory_snapshot_id},
            {"memory_view_digest", value.memory_view_digest},
            {"profile_revision_digest", value.profile_revision_digest},
            {"prompt_revision_digest", value.prompt_revision_digest},
            {"approval_decision_id", value.approval_decision_id},
            {"artifact_manifest_digest", value.artifact_manifest_digest},
            {"acceptance_report_digest", value.acceptance_report_digest},
            {"judge_report_digest", value.judge_report_digest},
            {"operations_snapshot_digest", value.operations_snapshot_digest}};
}

PinnedRevisions decode_pins(const json& value) {
    static const std::set<std::string> fields = {
        "intake_digest", "plan_digest", "acceptance_contract_digest", "memory_snapshot_id",
        "memory_view_digest", "profile_revision_digest", "prompt_revision_digest",
        "approval_decision_id", "artifact_manifest_digest", "acceptance_report_digest",
        "judge_report_digest", "operations_snapshot_digest"};
    if(!contracts::validate_object_fields(value, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, nullptr, "/payload/pins"))
        throw std::invalid_argument("invalid pins object");
    PinnedRevisions result;
    result.intake_digest = value.at("intake_digest").get<std::string>();
    result.plan_digest = value.at("plan_digest").get<std::string>();
    result.acceptance_contract_digest = value.at("acceptance_contract_digest").get<std::string>();
    result.memory_snapshot_id = value.at("memory_snapshot_id").get<std::string>();
    result.memory_view_digest = value.at("memory_view_digest").get<std::string>();
    result.profile_revision_digest = value.at("profile_revision_digest").get<std::string>();
    result.prompt_revision_digest = value.at("prompt_revision_digest").get<std::string>();
    result.approval_decision_id = value.at("approval_decision_id").get<std::string>();
    result.artifact_manifest_digest = value.at("artifact_manifest_digest").get<std::string>();
    result.acceptance_report_digest = value.at("acceptance_report_digest").get<std::string>();
    result.judge_report_digest = value.at("judge_report_digest").get<std::string>();
    result.operations_snapshot_digest = value.at("operations_snapshot_digest").get<std::string>();
    return result;
}

json encode_outbox(const HarnessOutboxEntry& value) {
    return {{"effect_id", value.effect_id}, {"stage", harness_stage_name(value.stage)},
            {"attempt", value.attempt}, {"idempotency_key", value.idempotency_key},
            {"request_digest", value.request_digest},
            {"state", outbox_state_name(value.state)},
            {"receipt_digest", value.receipt_digest}, {"error", value.error}};
}

HarnessOutboxEntry decode_outbox(const json& value) {
    static const std::set<std::string> fields = {
        "effect_id", "stage", "attempt", "idempotency_key", "request_digest", "state",
        "receipt_digest", "error"};
    if(!contracts::validate_object_fields(value, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, nullptr, "/payload/outbox"))
        throw std::invalid_argument("invalid outbox entry");
    HarnessOutboxEntry result;
    result.effect_id = value.at("effect_id").get<std::string>();
    const auto stage = harness_stage_from_name(value.at("stage").get<std::string>());
    const auto state = outbox_state_from_name(value.at("state").get<std::string>());
    if(!stage || !state) throw std::invalid_argument("unknown outbox enum value");
    result.stage = *stage;
    result.attempt = value.at("attempt").get<std::uint64_t>();
    result.idempotency_key = value.at("idempotency_key").get<std::string>();
    result.request_digest = value.at("request_digest").get<std::string>();
    result.state = *state;
    result.receipt_digest = value.at("receipt_digest").get<std::string>();
    result.error = value.at("error").get<std::string>();
    return result;
}

json encode_record(const HarnessStageRecord& value) {
    return {{"stage", harness_stage_name(value.stage)}, {"attempt", value.attempt},
            {"outcome", stage_outcome_name(value.outcome)}, {"effect_id", value.effect_id},
            {"invocation_manifest_digest", value.invocation_manifest_digest},
            {"output_digest", value.output_digest}, {"finding_ids", value.finding_ids},
            {"error_code", value.error_code}, {"error_message", value.error_message}};
}

HarnessStageRecord decode_record(const json& value) {
    static const std::set<std::string> fields = {
        "stage", "attempt", "outcome", "effect_id", "invocation_manifest_digest",
        "output_digest", "finding_ids", "error_code", "error_message"};
    if(!contracts::validate_object_fields(value, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, nullptr, "/payload/stage_records"))
        throw std::invalid_argument("invalid stage record");
    HarnessStageRecord result;
    const auto stage = harness_stage_from_name(value.at("stage").get<std::string>());
    const auto outcome = stage_outcome_from_name(value.at("outcome").get<std::string>());
    if(!stage || !outcome) throw std::invalid_argument("unknown stage record enum value");
    result.stage = *stage;
    result.attempt = value.at("attempt").get<std::uint64_t>();
    result.outcome = *outcome;
    result.effect_id = value.at("effect_id").get<std::string>();
    result.invocation_manifest_digest =
        value.at("invocation_manifest_digest").get<std::string>();
    result.output_digest = value.at("output_digest").get<std::string>();
    result.finding_ids = value.at("finding_ids").get<std::vector<std::string>>();
    result.error_code = value.at("error_code").get<std::string>();
    result.error_message = value.at("error_message").get<std::string>();
    return result;
}
}  // namespace

std::string harness_stage_name(HarnessStage value) { return enum_name(value, kStages); }
std::optional<HarnessStage> harness_stage_from_name(std::string_view value) {
    return enum_value<HarnessStage>(value, kStages);
}
std::string harness_state_name(HarnessState value) { return enum_name(value, kStates); }
std::optional<HarnessState> harness_state_from_name(std::string_view value) {
    return enum_value<HarnessState>(value, kStates);
}
std::string stage_outcome_name(StageOutcome value) { return enum_name(value, kOutcomes); }
std::optional<StageOutcome> stage_outcome_from_name(std::string_view value) {
    return enum_value<StageOutcome>(value, kOutcomes);
}
std::string outbox_state_name(OutboxState value) { return enum_name(value, kOutboxStates); }
std::optional<OutboxState> outbox_state_from_name(std::string_view value) {
    return enum_value<OutboxState>(value, kOutboxStates);
}

json encode(const HarnessCheckpoint& value) {
    json records = json::array();
    for(const auto& record : value.stage_records) records.push_back(encode_record(record));
    json outbox = json::array();
    for(const auto& entry : value.outbox) outbox.push_back(encode_outbox(entry));
    return contracts::make_typed_contract(value.metadata, "agent.phase4_harness_checkpoint/v1",
        {{"harness_id", value.harness_id}, {"revision", value.revision},
         {"state", harness_state_name(value.state)},
         {"next_stage", harness_stage_name(value.next_stage)},
         {"remediation_cycle", value.remediation_cycle},
         {"max_remediation_cycles", value.max_remediation_cycles},
         {"judge_required", value.judge_required}, {"pins", encode_pins(value.pins)},
         {"stage_records", std::move(records)}, {"outbox", std::move(outbox)},
         {"unresolved_findings", value.unresolved_findings},
         {"terminal_reason", value.terminal_reason}, {"updated_at", value.updated_at}});
}

std::optional<HarnessCheckpoint> decode_harness_checkpoint(
    const json& value, const contracts::ParseContext& context,
    std::vector<contracts::ContractIssue>* issues) {
    auto document = contracts::parse_typed_contract(
        value, "agent.phase4_harness_checkpoint/v1", context, issues);
    static const std::set<std::string> fields = {
        "harness_id", "revision", "state", "next_stage", "remediation_cycle",
        "max_remediation_cycles", "judge_required", "pins", "stage_records", "outbox",
        "unresolved_findings", "terminal_reason", "updated_at"};
    if(!document || !contracts::validate_object_fields(document->payload, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, issues, "/payload")) return std::nullopt;
    try {
        HarnessCheckpoint result;
        result.metadata = std::move(document->metadata);
        const auto& payload = document->payload;
        result.harness_id = payload.at("harness_id").get<std::string>();
        result.revision = payload.at("revision").get<std::uint64_t>();
        const auto state = harness_state_from_name(payload.at("state").get<std::string>());
        const auto stage = harness_stage_from_name(payload.at("next_stage").get<std::string>());
        if(!state || !stage) throw std::invalid_argument("unknown harness enum value");
        result.state = *state;
        result.next_stage = *stage;
        result.remediation_cycle = payload.at("remediation_cycle").get<std::uint64_t>();
        result.max_remediation_cycles =
            payload.at("max_remediation_cycles").get<std::uint64_t>();
        result.judge_required = payload.at("judge_required").get<bool>();
        result.pins = decode_pins(payload.at("pins"));
        for(const auto& item : payload.at("stage_records"))
            result.stage_records.push_back(decode_record(item));
        for(const auto& item : payload.at("outbox"))
            result.outbox.push_back(decode_outbox(item));
        result.unresolved_findings =
            payload.at("unresolved_findings").get<std::vector<std::string>>();
        result.terminal_reason = payload.at("terminal_reason").get<std::string>();
        result.updated_at = payload.at("updated_at").get<std::string>();
        if(result.harness_id.empty() || result.revision == 0 ||
           result.metadata.identity.tenant_id.empty() ||
           result.metadata.identity.task_id.empty())
            throw std::invalid_argument("harness identity and positive revision are required");
        return result;
    } catch(const std::exception& error) {
        contracts::append_issue(issues, "payload_decode_failed", "/payload", error.what());
        return std::nullopt;
    }
}

}  // namespace agent_framework::harness
