#ifndef AGENT_SKILL_SUPPLY_CHAIN_HPP
#define AGENT_SKILL_SUPPLY_CHAIN_HPP

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace agent_framework {

inline constexpr const char* kSkillSupplyChainInvalid = "skill_supply_chain_invalid";
inline constexpr const char* kSkillArchiveInvalid = "skill_archive_invalid";
inline constexpr const char* kSkillSignatureInvalid = "skill_signature_invalid";
inline constexpr const char* kSkillTrustDenied = "skill_trust_denied";
inline constexpr const char* kSkillRegistryInvalid = "skill_registry_invalid";

enum class SkillTrustRole { Package, Registry, Capability };

std::string skill_trust_role_name(SkillTrustRole role);
std::optional<SkillTrustRole> skill_trust_role_from_name(const std::string& value);
std::optional<std::string> skill_sha256_bytes(const std::string& value,
                                              std::string* error = nullptr);

struct SkillPackageMetadata {
    std::string api_version = "agent.taskflow/skill-package/v1";
    std::string package_id;
    std::string package_version;
    std::string archive_digest;
    std::string sbom_digest;
    std::string provenance_digest;
    std::uint64_t entry_count = 0;

    nlohmann::json to_json() const;
    static std::optional<SkillPackageMetadata> from_json(const nlohmann::json& value,
                                                         std::string* error = nullptr);
};

struct SkillSignatureEnvelope {
    std::string api_version = "agent.taskflow/skill-signature/v1";
    std::string algorithm = "Ed25519";
    std::string subject_kind = "package";
    std::string subject_digest;
    std::string key_id;
    std::string publisher;
    std::string source_uri;
    std::string sbom_digest;
    std::string provenance_digest;
    std::string signature;

    nlohmann::json to_json() const;
    static std::optional<SkillSignatureEnvelope> from_json(const nlohmann::json& value,
                                                           std::string* error = nullptr);
};

struct SkillSignatureResult {
    bool ok = false;
    std::string error;
    SkillSignatureEnvelope envelope;
};

struct SkillTrustedKey {
    std::string key_id;
    std::string publisher;
    std::string public_key_pem;
    std::set<SkillTrustRole> roles;
    std::vector<std::string> source_prefixes;
    std::int64_t not_before = 0;
    std::int64_t not_after = 0;
};

struct SkillTrustStore {
    std::string api_version = "agent.taskflow/skill-trust-store/v1";
    std::vector<SkillTrustedKey> keys;
    std::set<std::string> revoked_publishers;
    std::set<std::string> revoked_keys;
    std::set<std::string> revoked_packages;
    std::set<std::string> revoked_capabilities;

    nlohmann::json to_json() const;
    static std::optional<SkillTrustStore> from_json(const nlohmann::json& value,
                                                    std::string* error = nullptr);
};

std::string skill_signature_preimage(const SkillSignatureEnvelope& envelope);
std::optional<std::string> skill_public_key_id(const std::string& public_key_pem,
                                               std::string* error = nullptr);
SkillSignatureResult sign_skill_subject(SkillSignatureEnvelope envelope,
                                        const std::string& private_key_pem);
SkillSignatureResult verify_skill_signature(const SkillSignatureEnvelope& envelope,
                                            const SkillTrustStore& trust,
                                            SkillTrustRole required_role,
                                            std::int64_t now);

struct SkillRegistryArtifact {
    std::string package_id;
    std::string version;
    std::string digest;
    std::uint64_t size = 0;
    std::vector<std::string> mirrors;
    std::string signature_uri;
};

struct SkillRegistryIndex {
    std::string api_version = "agent.taskflow/skill-registry/v1";
    std::string registry_id;
    std::uint64_t generation = 0;
    std::vector<SkillRegistryArtifact> artifacts;

    nlohmann::json to_json() const;
    static std::optional<SkillRegistryIndex> from_json(const nlohmann::json& value,
                                                       std::string* error = nullptr);
};

} // namespace agent_framework

#endif
