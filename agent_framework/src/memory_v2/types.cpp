#include "agent/memory_v2/types.hpp"

#include <set>

namespace agent_framework::memory_v2 {
namespace {
using json = nlohmann::json;
using contracts::ContractIssue;

std::string level_string(MemoryLevel value) {
    static const char* values[] = {"system", "organization", "principal", "project", "task", "turn"};
    return values[static_cast<std::size_t>(value)];
}
MemoryLevel level_value(const std::string& value) {
    static const std::vector<std::string> values = {"system", "organization", "principal", "project", "task", "turn"};
    for(std::size_t i = 0; i < values.size(); ++i)
        if(values[i] == value) return static_cast<MemoryLevel>(i);
    throw std::invalid_argument("unknown memory level");
}
std::string authority_string(Authority value) {
    static const char* values[] = {"authoritative", "verified", "candidate", "observed", "derived"};
    return values[static_cast<std::size_t>(value)];
}
Authority authority_value(const std::string& value) {
    static const std::vector<std::string> values = {"authoritative", "verified", "candidate", "observed", "derived"};
    for(std::size_t i = 0; i < values.size(); ++i)
        if(values[i] == value) return static_cast<Authority>(i);
    throw std::invalid_argument("unknown memory authority");
}
std::string kind_string(MemoryKind value) {
    static const char* values[] = {"instruction", "semantic", "episodic", "procedural",
        "evidentiary", "working", "operational", "conversational"};
    return values[static_cast<std::size_t>(value)];
}
MemoryKind kind_value(const std::string& value) {
    static const std::vector<std::string> values = {"instruction", "semantic", "episodic", "procedural",
        "evidentiary", "working", "operational", "conversational"};
    for(std::size_t i=0;i<values.size();++i) if(values[i]==value) return static_cast<MemoryKind>(i);
    throw std::invalid_argument("unknown memory kind");
}
std::string status_string(MemoryStatus value) {
    static const char* values[] = {"raw", "candidate", "verified", "authoritative", "rejected", "superseded", "tombstoned", "expired"};
    return values[static_cast<std::size_t>(value)];
}
MemoryStatus status_value(const std::string& value) {
    static const std::vector<std::string> values = {"raw", "candidate", "verified", "authoritative", "rejected", "superseded", "tombstoned", "expired"};
    for(std::size_t i=0;i<values.size();++i) if(values[i]==value) return static_cast<MemoryStatus>(i);
    throw std::invalid_argument("unknown memory status");
}
json scope_json(const MemoryScope& value) {
    return {{"tenant_id", value.tenant_id}, {"organization_id", value.organization_id},
            {"principal_id", value.principal_id}, {"agent_id", value.agent_id},
            {"project_id", value.project_id}, {"workspace_id", value.workspace_id},
            {"path_scope", value.path_scope},
            {"task_id", value.task_id}, {"run_id", value.run_id}, {"turn_id", value.turn_id},
            {"level", level_string(value.level)}};
}
MemoryScope scope_value(const json& value) {
    return {value.at("tenant_id").get<std::string>(),
            value.at("organization_id").get<std::string>(),
            value.at("principal_id").get<std::string>(),
            value.at("agent_id").get<std::string>(),
            value.at("project_id").get<std::string>(),
            value.at("workspace_id").get<std::string>(), value.at("path_scope").get<std::string>(),
            value.at("task_id").get<std::string>(),
            value.at("run_id").get<std::string>(), value.at("turn_id").get<std::string>(),
            level_value(value.at("level").get<std::string>())};
}
json selection_json(const MemorySelection& value) {
    return {{"record_id", value.record_id}, {"revision", value.revision}, {"digest", value.digest},
            {"reason", value.reason}, {"bytes", value.bytes}};
}
MemorySelection selection_value(const json& value) {
    return {value.at("record_id").get<std::string>(), value.at("revision").get<std::uint64_t>(),
            value.at("digest").get<std::string>(), value.at("reason").get<std::string>(),
            value.at("bytes").get<std::uint64_t>()};
}
template <typename T, typename Builder>
std::optional<T> decode_value(const json& value, const char* kind,
                              const std::set<std::string>& fields,
                              const contracts::ParseContext& context,
                              std::vector<ContractIssue>* issues, Builder builder) {
    auto document = contracts::parse_typed_contract(value, kind, context, issues, false);
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

json encode(const MemoryRecord& value) {
    return contracts::make_typed_contract(value.metadata, "agent.memory_record/v1",
        {{"record_id", value.record_id}, {"revision", value.revision}, {"scope", scope_json(value.scope)},
         {"kind", kind_string(value.kind)}, {"authority", authority_string(value.authority)},
         {"status", status_string(value.status)}, {"source_kind", value.source_kind},
         {"source_locator", value.source_locator}, {"source_digest", value.source_digest},
         {"content_type", value.content_type}, {"content", value.content},
         {"trust_class", value.trust_class}, {"freshness_deadline", value.freshness_deadline},
         {"sensitivity", value.sensitivity}, {"purpose", value.purpose},
         {"acl_principals", value.acl_principals}, {"supports", value.supports},
         {"conflicts_with", value.conflicts_with}, {"supersedes_id", value.supersedes_id},
         {"event_time", value.event_time}, {"ingest_time", value.ingest_time},
         {"valid_from", value.valid_from}, {"valid_until", value.valid_until},
         {"retention_until", value.retention_until}, {"legal_hold", value.legal_hold},
         {"created_at", value.created_at}});
}
json encode(const MemorySnapshot& value) {
    return contracts::make_typed_contract(value.metadata, "agent.memory_snapshot/v1",
        {{"snapshot_id", value.snapshot_id}, {"provider_generation", value.provider_generation},
         {"record_revision_digests", value.record_revision_digests},
         {"policy_revision", value.policy_revision}, {"created_at", value.created_at}});
}
json encode(const MemoryViewSpec& value) {
    json levels = json::array(), kinds = json::array();
    for(auto item : value.allowed_levels) levels.push_back(level_string(item));
    for(auto item : value.allowed_kinds) kinds.push_back(kind_string(item));
    return contracts::make_typed_contract(value.metadata, "agent.memory_view_spec/v1",
        {{"workflow_phase", value.workflow_phase}, {"subject", scope_json(value.subject)},
         {"allowed_levels", std::move(levels)}, {"allowed_kinds", std::move(kinds)},
         {"authority_floor", authority_string(value.authority_floor)},
         {"mandatory_record_ids", value.mandatory_record_ids}, {"byte_budget", value.byte_budget},
         {"token_budget", value.token_budget}, {"conflict_policy", value.conflict_policy},
         {"include_procedural_skills", value.include_procedural_skills},
         {"exclude_unverified_executor_claims", value.exclude_unverified_executor_claims}});
}
json encode(const MemoryViewManifest& value) {
    json selected = json::array(), excluded = json::array();
    for(const auto& item : value.selected) selected.push_back(selection_json(item));
    for(const auto& item : value.excluded) excluded.push_back(selection_json(item));
    return contracts::make_typed_contract(value.metadata, "agent.memory_view_manifest/v1",
        {{"snapshot_id", value.snapshot_id}, {"snapshot_digest", value.snapshot_digest},
         {"view_spec_digest", value.view_spec_digest}, {"view_digest", value.view_digest},
         {"policy_revision", value.policy_revision}, {"provider_generation", value.provider_generation},
         {"selected", std::move(selected)}, {"excluded", std::move(excluded)},
         {"conflict_ids", value.conflict_ids}, {"redactions", value.redactions}});
}
json encode(const MemoryConflict& value) {
    return contracts::make_typed_contract(value.metadata, "agent.memory_conflict/v1",
        {{"conflict_id", value.conflict_id},
         {"record_revision_digests", value.record_revision_digests},
         {"resolution_state", value.resolution_state}, {"resolution_reason", value.resolution_reason},
         {"approval_id", value.approval_id}});
}

std::optional<MemoryRecord> decode_memory_record(const json& value,
    const contracts::ParseContext& context, std::vector<ContractIssue>* issues) {
    static const std::set<std::string> fields = {"record_id", "revision", "scope", "kind", "authority", "status",
        "source_kind", "source_locator", "source_digest", "content_type", "content", "trust_class",
        "freshness_deadline", "sensitivity", "purpose", "acl_principals", "supports", "conflicts_with",
        "supersedes_id", "event_time", "ingest_time", "valid_from", "valid_until", "retention_until",
        "legal_hold", "created_at"};
    return decode_value<MemoryRecord>(value, "agent.memory_record/v1", fields, context, issues,
        [](const json& p) {
            MemoryRecord r;
            r.record_id = p.at("record_id").get<std::string>();
            r.revision = p.at("revision").get<std::uint64_t>();
            r.scope = scope_value(p.at("scope"));
            r.kind = kind_value(p.at("kind").get<std::string>());
            r.authority = authority_value(p.at("authority").get<std::string>());
            r.status = status_value(p.at("status").get<std::string>());
            r.source_kind = p.at("source_kind").get<std::string>();
            r.source_locator = p.at("source_locator").get<std::string>();
            r.source_digest = p.at("source_digest").get<std::string>();
            r.content_type = p.at("content_type").get<std::string>();
            r.content = p.at("content");
            r.trust_class = p.at("trust_class").get<std::string>();
            r.freshness_deadline = p.at("freshness_deadline").get<std::string>();
            r.sensitivity = p.at("sensitivity").get<std::string>();
            r.purpose = p.at("purpose").get<std::string>();
            r.acl_principals = p.at("acl_principals").get<std::vector<std::string>>();
            r.supports = p.at("supports").get<std::vector<std::string>>();
            r.conflicts_with = p.at("conflicts_with").get<std::vector<std::string>>();
            r.supersedes_id = p.at("supersedes_id").get<std::string>();
            r.event_time = p.at("event_time").get<std::string>();
            r.ingest_time = p.at("ingest_time").get<std::string>();
            r.valid_from = p.at("valid_from").get<std::string>();
            r.valid_until = p.at("valid_until").get<std::string>();
            r.retention_until = p.at("retention_until").get<std::string>();
            r.legal_hold = p.at("legal_hold").get<bool>();
            r.created_at = p.at("created_at").get<std::string>();
            return r;
        });
}
std::optional<MemorySnapshot> decode_memory_snapshot(const json& value,
    const contracts::ParseContext& context, std::vector<ContractIssue>* issues) {
    static const std::set<std::string> fields = {"snapshot_id", "provider_generation",
        "record_revision_digests", "policy_revision", "created_at"};
    return decode_value<MemorySnapshot>(value, "agent.memory_snapshot/v1", fields, context, issues,
        [](const json& p) {
            MemorySnapshot r;
            r.snapshot_id = p.at("snapshot_id").get<std::string>();
            r.provider_generation = p.at("provider_generation").get<std::uint64_t>();
            r.record_revision_digests = p.at("record_revision_digests").get<std::vector<std::string>>();
            r.policy_revision = p.at("policy_revision").get<std::string>();
            r.created_at = p.at("created_at").get<std::string>();
            return r;
        });
}
std::optional<MemoryViewSpec> decode_memory_view_spec(const json& value,
    const contracts::ParseContext& context, std::vector<ContractIssue>* issues) {
    static const std::set<std::string> fields = {"workflow_phase", "subject", "allowed_levels",
        "allowed_kinds", "authority_floor", "mandatory_record_ids", "byte_budget", "token_budget", "conflict_policy",
        "include_procedural_skills", "exclude_unverified_executor_claims"};
    return decode_value<MemoryViewSpec>(value, "agent.memory_view_spec/v1", fields, context, issues,
        [](const json& p) {
            MemoryViewSpec r;
            r.workflow_phase = p.at("workflow_phase").get<std::string>();
            r.subject = scope_value(p.at("subject"));
            for(const auto& item : p.at("allowed_levels")) r.allowed_levels.push_back(level_value(item.get<std::string>()));
            for(const auto& item : p.at("allowed_kinds")) r.allowed_kinds.push_back(kind_value(item.get<std::string>()));
            r.authority_floor = authority_value(p.at("authority_floor").get<std::string>());
            r.mandatory_record_ids = p.at("mandatory_record_ids").get<std::vector<std::string>>();
            r.byte_budget = p.at("byte_budget").get<std::uint64_t>();
            r.token_budget = p.at("token_budget").get<std::uint64_t>();
            r.conflict_policy = p.at("conflict_policy").get<std::string>();
            r.include_procedural_skills = p.at("include_procedural_skills").get<bool>();
            r.exclude_unverified_executor_claims =
                p.at("exclude_unverified_executor_claims").get<bool>();
            return r;
        });
}
std::optional<MemoryViewManifest> decode_memory_view_manifest(const json& value,
    const contracts::ParseContext& context, std::vector<ContractIssue>* issues) {
    static const std::set<std::string> fields = {"snapshot_id", "snapshot_digest", "view_spec_digest",
        "view_digest", "policy_revision", "provider_generation", "selected", "excluded",
        "conflict_ids", "redactions"};
    return decode_value<MemoryViewManifest>(value, "agent.memory_view_manifest/v1", fields, context, issues,
        [](const json& p) {
            MemoryViewManifest r;
            r.snapshot_id = p.at("snapshot_id").get<std::string>();
            r.snapshot_digest = p.at("snapshot_digest").get<std::string>();
            r.view_spec_digest = p.at("view_spec_digest").get<std::string>();
            r.view_digest = p.at("view_digest").get<std::string>();
            r.policy_revision = p.at("policy_revision").get<std::string>();
            r.provider_generation = p.at("provider_generation").get<std::uint64_t>();
            for(const auto& item : p.at("selected")) r.selected.push_back(selection_value(item));
            for(const auto& item : p.at("excluded")) r.excluded.push_back(selection_value(item));
            r.conflict_ids = p.at("conflict_ids").get<std::vector<std::string>>();
            r.redactions = p.at("redactions").get<std::vector<std::string>>();
            return r;
        });
}
std::optional<MemoryConflict> decode_memory_conflict(const json& value,
    const contracts::ParseContext& context, std::vector<ContractIssue>* issues) {
    static const std::set<std::string> fields = {"conflict_id", "record_revision_digests",
        "resolution_state", "resolution_reason", "approval_id"};
    return decode_value<MemoryConflict>(value, "agent.memory_conflict/v1", fields, context, issues,
        [](const json& p) {
            MemoryConflict r;
            r.conflict_id = p.at("conflict_id").get<std::string>();
            r.record_revision_digests = p.at("record_revision_digests").get<std::vector<std::string>>();
            r.resolution_state = p.at("resolution_state").get<std::string>();
            r.resolution_reason = p.at("resolution_reason").get<std::string>();
            r.approval_id = p.at("approval_id").get<std::string>();
            return r;
        });
}
}  // namespace agent_framework::memory_v2
