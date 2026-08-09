#pragma once

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace agent_framework::contracts {

inline constexpr int kPhase4SchemaVersion = 1;

enum class UnknownFieldPolicy {
    Reject,
    Preserve,
};

struct ContractIdentity {
    std::string tenant_id;
    std::string organization_id;
    std::string principal_id;
    std::string project_id;
    std::string task_id;
    std::string run_id;
    std::string plan_id;
    std::string memory_id;
};

struct ContractMetadata {
    int schema_version{kPhase4SchemaVersion};
    ContractIdentity identity;
    std::string canonical_digest;
    nlohmann::json extensions = nlohmann::json::object();
};

struct ContractIssue {
    std::string code;
    std::string path;
    std::string message;
};

struct ParseContext {
    UnknownFieldPolicy unknown_fields{UnknownFieldPolicy::Reject};
    bool verify_digest{true};
};

struct RedactionRule {
    std::string json_pointer;
    std::string replacement{"[REDACTED]"};
};

using SchemaMigration = std::function<nlohmann::json(const nlohmann::json&)>;

class SchemaMigrator {
public:
    bool register_step(int from_version, int to_version, SchemaMigration migration);
    std::optional<nlohmann::json> migrate(const nlohmann::json& value,
                                          int target_version,
                                          std::vector<ContractIssue>* issues = nullptr) const;

private:
    std::map<std::pair<int, int>, SchemaMigration> migrations_;
};

nlohmann::json identity_to_json(const ContractIdentity& identity);
std::optional<ContractIdentity> identity_from_json(const nlohmann::json& value,
                                                   std::vector<ContractIssue>* issues = nullptr,
                                                   std::string_view path = "/identity");

std::string canonical_json(const nlohmann::json& value);
std::optional<std::string> canonical_digest(const nlohmann::json& value,
                                            std::string* error = nullptr);
std::optional<std::string> embedded_digest(const nlohmann::json& value,
                                           std::string* error = nullptr);
bool verify_embedded_digest(const nlohmann::json& value,
                            std::vector<ContractIssue>* issues = nullptr);

nlohmann::json redact_json(nlohmann::json value, const std::vector<RedactionRule>& rules,
                           std::vector<ContractIssue>* issues = nullptr);

bool validate_object_fields(const nlohmann::json& value,
                            const std::set<std::string>& required,
                            const std::set<std::string>& allowed,
                            UnknownFieldPolicy policy,
                            nlohmann::json* extensions,
                            std::vector<ContractIssue>* issues,
                            std::string_view path = "");

bool validate_metadata(const ContractMetadata& metadata,
                       std::vector<ContractIssue>* issues = nullptr,
                       bool require_task = true);
nlohmann::json metadata_to_json(const ContractMetadata& metadata);
std::optional<ContractMetadata> metadata_from_json(const nlohmann::json& value,
                                                   const ParseContext& context,
                                                   std::vector<ContractIssue>* issues = nullptr);
nlohmann::json make_contract_json(const ContractMetadata& metadata,
                                  nlohmann::json payload);
std::optional<ContractMetadata> metadata_from_contract_json(
    const nlohmann::json& value, const std::set<std::string>& required_payload_fields,
    const std::set<std::string>& allowed_payload_fields, const ParseContext& context,
    std::vector<ContractIssue>* issues = nullptr, bool require_task = true);
nlohmann::json make_typed_contract(const ContractMetadata& metadata,
                                   std::string_view kind,
                                   nlohmann::json payload);
struct TypedContractDocument {
    ContractMetadata metadata;
    std::string kind;
    nlohmann::json payload = nlohmann::json::object();
};
std::optional<TypedContractDocument> parse_typed_contract(
    const nlohmann::json& value, std::string_view expected_kind,
    const ParseContext& context = {}, std::vector<ContractIssue>* issues = nullptr,
    bool require_task = true);

void append_issue(std::vector<ContractIssue>* issues, std::string code,
                  std::string path, std::string message);

}  // namespace agent_framework::contracts
