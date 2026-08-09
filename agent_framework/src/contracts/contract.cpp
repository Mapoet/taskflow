#include "agent/contracts/contract.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

#include "agent/skills/skill_supply_chain.hpp"

namespace agent_framework::contracts {
namespace {

using json = nlohmann::json;

bool finite_numbers(const json& value) {
    if(value.is_number_float()) return std::isfinite(value.get<double>());
    if(value.is_array()) {
        for(const auto& item : value) if(!finite_numbers(item)) return false;
    } else if(value.is_object()) {
        for(const auto& [key, item] : value.items()) {
            (void)key;
            if(!finite_numbers(item)) return false;
        }
    }
    return true;
}

std::string child_path(std::string_view base, std::string_view child) {
    std::string result(base);
    result.push_back('/');
    result.append(child);
    return result;
}

std::string string_field(const json& value, const char* name,
                         std::vector<ContractIssue>* issues, std::string_view path) {
    const auto it = value.find(name);
    if(it == value.end()) return {};
    if(!it->is_string()) {
        append_issue(issues, "invalid_type", child_path(path, name), "expected string");
        return {};
    }
    return it->get<std::string>();
}

}  // namespace

void append_issue(std::vector<ContractIssue>* issues, std::string code,
                  std::string path, std::string message) {
    if(issues) issues->push_back({std::move(code), std::move(path), std::move(message)});
}

bool SchemaMigrator::register_step(int from_version, int to_version,
                                   SchemaMigration migration) {
    if(from_version < 0 || to_version != from_version + 1 || !migration) return false;
    return migrations_.emplace(std::make_pair(from_version, to_version),
                               std::move(migration)).second;
}

std::optional<json> SchemaMigrator::migrate(const json& value, int target_version,
                                            std::vector<ContractIssue>* issues) const {
    if(!value.is_object() || !value.contains("schema_version") ||
       !value.at("schema_version").is_number_integer()) {
        append_issue(issues, "schema_version_missing", "/schema_version",
                     "schema_version must be an integer");
        return std::nullopt;
    }
    json current = value;
    int version = current.at("schema_version").get<int>();
    if(version > target_version) {
        append_issue(issues, "future_schema_version", "/schema_version",
                     "future schema versions are rejected");
        return std::nullopt;
    }
    while(version < target_version) {
        const auto step = migrations_.find({version, version + 1});
        if(step == migrations_.end()) {
            append_issue(issues, "migration_missing", "/schema_version",
                         "no contiguous schema migration is registered");
            return std::nullopt;
        }
        try {
            current = step->second(current);
        } catch(const std::exception& error) {
            append_issue(issues, "migration_failed", "/schema_version", error.what());
            return std::nullopt;
        }
        if(!current.is_object()) {
            append_issue(issues, "migration_invalid_output", "",
                         "migration must return a JSON object");
            return std::nullopt;
        }
        current["schema_version"] = ++version;
        current.erase("canonical_digest");
        const auto digest = embedded_digest(current);
        if(!digest) return std::nullopt;
        current["canonical_digest"] = *digest;
    }
    return current;
}

json identity_to_json(const ContractIdentity& identity) {
    return {{"tenant_id", identity.tenant_id},
            {"organization_id", identity.organization_id},
            {"principal_id", identity.principal_id},
            {"project_id", identity.project_id},
            {"task_id", identity.task_id},
            {"run_id", identity.run_id},
            {"plan_id", identity.plan_id},
            {"memory_id", identity.memory_id}};
}

std::optional<ContractIdentity> identity_from_json(const json& value,
                                                   std::vector<ContractIssue>* issues,
                                                   std::string_view path) {
    if(!value.is_object()) {
        append_issue(issues, "invalid_type", std::string(path), "expected identity object");
        return std::nullopt;
    }
    static const std::set<std::string> fields = {
        "tenant_id", "organization_id", "principal_id", "project_id",
        "task_id", "run_id", "plan_id", "memory_id"};
    bool ok = validate_object_fields(value, {}, fields, UnknownFieldPolicy::Reject,
                                     nullptr, issues, path);
    ContractIdentity identity;
    identity.tenant_id = string_field(value, "tenant_id", issues, path);
    identity.organization_id = string_field(value, "organization_id", issues, path);
    identity.principal_id = string_field(value, "principal_id", issues, path);
    identity.project_id = string_field(value, "project_id", issues, path);
    identity.task_id = string_field(value, "task_id", issues, path);
    identity.run_id = string_field(value, "run_id", issues, path);
    identity.plan_id = string_field(value, "plan_id", issues, path);
    identity.memory_id = string_field(value, "memory_id", issues, path);
    if(issues && !issues->empty()) ok = false;
    return ok ? std::optional<ContractIdentity>(std::move(identity)) : std::nullopt;
}

std::string canonical_json(const json& value) {
    if(!finite_numbers(value)) throw std::invalid_argument("non-finite JSON number");
    return value.dump(-1, ' ', false, json::error_handler_t::strict);
}

std::optional<std::string> canonical_digest(const json& value, std::string* error) {
    try {
        json normalized = value;
        if(normalized.is_object()) normalized.erase("canonical_digest");
        return skill_sha256_bytes(canonical_json(normalized), error);
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return std::nullopt;
    }
}

std::optional<std::string> embedded_digest(const json& value, std::string* error) {
    auto digest = canonical_digest(value, error);
    if(!digest) return std::nullopt;
    return std::string("sha256:") + *digest;
}

bool verify_embedded_digest(const json& value, std::vector<ContractIssue>* issues) {
    if(!value.is_object() || !value.contains("canonical_digest") ||
       !value.at("canonical_digest").is_string()) {
        append_issue(issues, "digest_missing", "/canonical_digest",
                     "canonical_digest is required");
        return false;
    }
    const auto actual = embedded_digest(value);
    if(!actual || value.at("canonical_digest").get<std::string>() != *actual) {
        append_issue(issues, "digest_mismatch", "/canonical_digest",
                     "canonical digest does not match the contract payload");
        return false;
    }
    return true;
}

json redact_json(json value, const std::vector<RedactionRule>& rules,
                 std::vector<ContractIssue>* issues) {
    for(const auto& rule : rules) {
        try {
            const json::json_pointer pointer(rule.json_pointer);
            if(value.contains(pointer)) value[pointer] = rule.replacement;
        } catch(const std::exception& exception) {
            append_issue(issues, "invalid_redaction_pointer", rule.json_pointer,
                         exception.what());
        }
    }
    if(value.is_object() && value.contains("canonical_digest")) {
        value.erase("canonical_digest");
        if(const auto digest = embedded_digest(value)) value["canonical_digest"] = *digest;
    }
    return value;
}

bool validate_object_fields(const json& value, const std::set<std::string>& required,
                            const std::set<std::string>& allowed,
                            UnknownFieldPolicy policy, json* extensions,
                            std::vector<ContractIssue>* issues, std::string_view path) {
    if(!value.is_object()) {
        append_issue(issues, "invalid_type", std::string(path), "expected object");
        return false;
    }
    bool valid = true;
    for(const auto& field : required) {
        if(!value.contains(field)) {
            append_issue(issues, "required_field_missing", child_path(path, field),
                         "required field is missing");
            valid = false;
        }
    }
    for(const auto& [field, field_value] : value.items()) {
        if(allowed.count(field) != 0) continue;
        if(policy == UnknownFieldPolicy::Reject) {
            append_issue(issues, "unknown_field", child_path(path, field),
                         "unknown fields are rejected by this schema");
            valid = false;
        } else if(extensions) {
            (*extensions)[field] = field_value;
        }
    }
    return valid;
}

bool validate_metadata(const ContractMetadata& metadata,
                       std::vector<ContractIssue>* issues, bool require_task) {
    bool valid = true;
    if(metadata.schema_version != kPhase4SchemaVersion) {
        append_issue(issues, metadata.schema_version > kPhase4SchemaVersion
                                 ? "future_schema_version" : "unsupported_schema_version",
                     "/schema_version", "only Phase 4 schema version 1 is accepted");
        valid = false;
    }
    if(metadata.identity.tenant_id.empty()) {
        append_issue(issues, "identity_missing", "/identity/tenant_id",
                     "tenant identity is required");
        valid = false;
    }
    if(require_task && metadata.identity.task_id.empty()) {
        append_issue(issues, "identity_missing", "/identity/task_id",
                     "task identity is required");
        valid = false;
    }
    if(!metadata.extensions.is_object()) {
        append_issue(issues, "invalid_type", "/extensions", "extensions must be an object");
        valid = false;
    }
    return valid;
}

json metadata_to_json(const ContractMetadata& metadata) {
    return {{"schema_version", metadata.schema_version},
            {"identity", identity_to_json(metadata.identity)},
            {"canonical_digest", metadata.canonical_digest},
            {"extensions", metadata.extensions}};
}

std::optional<ContractMetadata> metadata_from_json(const json& value,
                                                   const ParseContext& context,
                                                   std::vector<ContractIssue>* issues) {
    static const std::set<std::string> required = {"schema_version", "identity",
                                                   "canonical_digest"};
    static const std::set<std::string> allowed = {"schema_version", "identity",
                                                  "canonical_digest", "extensions"};
    ContractMetadata metadata;
    bool valid = validate_object_fields(value, required, allowed, context.unknown_fields,
                                        &metadata.extensions, issues);
    if(!valid) return std::nullopt;
    if(!value.at("schema_version").is_number_integer()) {
        append_issue(issues, "invalid_type", "/schema_version", "expected integer");
        return std::nullopt;
    }
    metadata.schema_version = value.at("schema_version").get<int>();
    auto identity = identity_from_json(value.at("identity"), issues);
    if(!identity) return std::nullopt;
    metadata.identity = std::move(*identity);
    if(!value.at("canonical_digest").is_string()) {
        append_issue(issues, "invalid_type", "/canonical_digest", "expected string");
        return std::nullopt;
    }
    metadata.canonical_digest = value.at("canonical_digest").get<std::string>();
    if(value.contains("extensions")) {
        if(!value.at("extensions").is_object()) {
            append_issue(issues, "invalid_type", "/extensions", "expected object");
            return std::nullopt;
        }
        for(const auto& [key, extension] : value.at("extensions").items())
            metadata.extensions[key] = extension;
    }
    if(!validate_metadata(metadata, issues)) return std::nullopt;
    if(context.verify_digest && !verify_embedded_digest(value, issues)) return std::nullopt;
    return metadata;
}

json make_contract_json(const ContractMetadata& metadata, json payload) {
    if(!payload.is_object()) throw std::invalid_argument("contract payload must be an object");
    payload["schema_version"] = metadata.schema_version;
    payload["identity"] = identity_to_json(metadata.identity);
    payload["extensions"] = metadata.extensions;
    payload.erase("canonical_digest");
    const auto digest = embedded_digest(payload);
    if(!digest) throw std::runtime_error("unable to compute contract digest");
    payload["canonical_digest"] = *digest;
    return payload;
}

std::optional<ContractMetadata> metadata_from_contract_json(
    const json& value, const std::set<std::string>& required_payload_fields,
    const std::set<std::string>& allowed_payload_fields, const ParseContext& context,
    std::vector<ContractIssue>* issues, bool require_task) {
    std::set<std::string> required = required_payload_fields;
    required.insert("schema_version");
    required.insert("identity");
    required.insert("canonical_digest");
    std::set<std::string> allowed = allowed_payload_fields;
    allowed.insert("schema_version");
    allowed.insert("identity");
    allowed.insert("canonical_digest");
    allowed.insert("extensions");

    ContractMetadata metadata;
    if(!validate_object_fields(value, required, allowed, context.unknown_fields,
                               &metadata.extensions, issues)) return std::nullopt;
    if(!value.at("schema_version").is_number_integer()) {
        append_issue(issues, "invalid_type", "/schema_version", "expected integer");
        return std::nullopt;
    }
    metadata.schema_version = value.at("schema_version").get<int>();
    auto identity = identity_from_json(value.at("identity"), issues);
    if(!identity) return std::nullopt;
    metadata.identity = std::move(*identity);
    if(!value.at("canonical_digest").is_string()) {
        append_issue(issues, "invalid_type", "/canonical_digest", "expected string");
        return std::nullopt;
    }
    metadata.canonical_digest = value.at("canonical_digest").get<std::string>();
    if(value.contains("extensions")) {
        if(!value.at("extensions").is_object()) {
            append_issue(issues, "invalid_type", "/extensions", "expected object");
            return std::nullopt;
        }
        for(const auto& [key, extension] : value.at("extensions").items())
            metadata.extensions[key] = extension;
    }
    if(!validate_metadata(metadata, issues, require_task)) return std::nullopt;
    if(context.verify_digest && !verify_embedded_digest(value, issues)) return std::nullopt;
    return metadata;
}

json make_typed_contract(const ContractMetadata& metadata, std::string_view kind,
                         json payload) {
    if(kind.empty()) throw std::invalid_argument("contract kind must not be empty");
    if(!payload.is_object()) throw std::invalid_argument("typed contract payload must be an object");
    return make_contract_json(metadata, {{"kind", kind}, {"payload", std::move(payload)}});
}

std::optional<TypedContractDocument> parse_typed_contract(
    const json& value, std::string_view expected_kind, const ParseContext& context,
    std::vector<ContractIssue>* issues, bool require_task) {
    auto metadata = metadata_from_contract_json(value, {"kind", "payload"},
                                                {"kind", "payload"}, context,
                                                issues, require_task);
    if(!metadata) return std::nullopt;
    if(!value.at("kind").is_string()) {
        append_issue(issues, "invalid_type", "/kind", "expected string");
        return std::nullopt;
    }
    const auto kind = value.at("kind").get<std::string>();
    if(kind != expected_kind) {
        append_issue(issues, "contract_kind_mismatch", "/kind",
                     "contract kind does not match the requested schema");
        return std::nullopt;
    }
    if(!value.at("payload").is_object()) {
        append_issue(issues, "invalid_type", "/payload", "expected object");
        return std::nullopt;
    }
    return TypedContractDocument{std::move(*metadata), kind, value.at("payload")};
}

}  // namespace agent_framework::contracts
