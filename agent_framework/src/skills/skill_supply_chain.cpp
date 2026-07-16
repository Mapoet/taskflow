#include "agent/skill_supply_chain.hpp"

#include <algorithm>

namespace agent_framework {
namespace {

using nlohmann::json;

void fail(std::string* error, const std::string& detail) {
    if(error) *error = std::string(kSkillSupplyChainInvalid) + ": " + detail;
}

bool is_sha256(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool required_string(const json& value, const char* key, std::string& output,
                     std::string* error) {
    if(!value.contains(key) || !value.at(key).is_string() || value.at(key).get<std::string>().empty()) {
        fail(error, std::string("missing or invalid field '") + key + "'");
        return false;
    }
    output = value.at(key).get<std::string>();
    return true;
}

bool required_digest(const json& value, const char* key, std::string& output,
                     std::string* error) {
    if(!required_string(value, key, output, error)) return false;
    if(!is_sha256(output)) {
        fail(error, std::string("field '") + key + "' must be lowercase SHA-256");
        return false;
    }
    return true;
}

} // namespace

std::string skill_trust_role_name(SkillTrustRole role) {
    return role == SkillTrustRole::Package ? "package" : "registry";
}

std::optional<SkillTrustRole> skill_trust_role_from_name(const std::string& value) {
    if(value == "package") return SkillTrustRole::Package;
    if(value == "registry") return SkillTrustRole::Registry;
    return std::nullopt;
}

json SkillPackageMetadata::to_json() const {
    return {{"apiVersion", api_version}, {"packageId", package_id},
            {"packageVersion", package_version}, {"archiveDigest", archive_digest},
            {"sbomDigest", sbom_digest}, {"provenanceDigest", provenance_digest},
            {"entryCount", entry_count}};
}

std::optional<SkillPackageMetadata> SkillPackageMetadata::from_json(const json& value,
                                                                    std::string* error) {
    SkillPackageMetadata out;
    if(!value.is_object() || !required_string(value, "apiVersion", out.api_version, error) ||
       out.api_version != "agent.taskflow/skill-package/v1" ||
       !required_string(value, "packageId", out.package_id, error) ||
       !required_string(value, "packageVersion", out.package_version, error) ||
       !required_digest(value, "archiveDigest", out.archive_digest, error) ||
       !required_digest(value, "sbomDigest", out.sbom_digest, error) ||
       !required_digest(value, "provenanceDigest", out.provenance_digest, error) ||
       !value.contains("entryCount") || !value.at("entryCount").is_number_unsigned()) {
        if(error && error->empty()) fail(error, "invalid package metadata");
        return std::nullopt;
    }
    out.entry_count = value.at("entryCount").get<std::uint64_t>();
    return out;
}

json SkillSignatureEnvelope::to_json() const {
    return {{"apiVersion", api_version}, {"algorithm", algorithm},
            {"subjectKind", subject_kind}, {"subjectDigest", subject_digest},
            {"keyId", key_id}, {"publisher", publisher}, {"sourceUri", source_uri},
            {"sbomDigest", sbom_digest}, {"provenanceDigest", provenance_digest},
            {"signature", signature}};
}

std::optional<SkillSignatureEnvelope> SkillSignatureEnvelope::from_json(const json& value,
                                                                        std::string* error) {
    SkillSignatureEnvelope out;
    if(!value.is_object() || !required_string(value, "apiVersion", out.api_version, error) ||
       out.api_version != "agent.taskflow/skill-signature/v1" ||
       !required_string(value, "algorithm", out.algorithm, error) || out.algorithm != "Ed25519" ||
       !required_string(value, "subjectKind", out.subject_kind, error) ||
       (out.subject_kind != "package" && out.subject_kind != "registry") ||
       !required_digest(value, "subjectDigest", out.subject_digest, error) ||
       !required_digest(value, "keyId", out.key_id, error) ||
       !required_string(value, "publisher", out.publisher, error) ||
       !required_string(value, "sourceUri", out.source_uri, error) ||
       !required_digest(value, "sbomDigest", out.sbom_digest, error) ||
       !required_digest(value, "provenanceDigest", out.provenance_digest, error) ||
       !required_string(value, "signature", out.signature, error)) return std::nullopt;
    return out;
}

json SkillTrustStore::to_json() const {
    json serialized_keys = json::array();
    for(const auto& key : keys) {
        json roles = json::array();
        for(auto role : key.roles) roles.push_back(skill_trust_role_name(role));
        serialized_keys.push_back({{"keyId", key.key_id}, {"publisher", key.publisher},
                                   {"publicKeyPem", key.public_key_pem}, {"roles", roles},
                                   {"sourcePrefixes", key.source_prefixes},
                                   {"notBefore", key.not_before}, {"notAfter", key.not_after}});
    }
    return {{"apiVersion", api_version}, {"keys", serialized_keys},
            {"revokedPublishers", revoked_publishers}, {"revokedKeys", revoked_keys},
            {"revokedPackages", revoked_packages}};
}

std::optional<SkillTrustStore> SkillTrustStore::from_json(const json& value, std::string* error) {
    SkillTrustStore out;
    if(!value.is_object() || !required_string(value, "apiVersion", out.api_version, error) ||
       out.api_version != "agent.taskflow/skill-trust-store/v1" ||
       !value.contains("keys") || !value.at("keys").is_array()) {
        if(error && error->empty()) fail(error, "invalid trust store");
        return std::nullopt;
    }
    try {
        for(const auto& item : value.at("keys")) {
            SkillTrustedKey key;
            if(!required_digest(item, "keyId", key.key_id, error) ||
               !required_string(item, "publisher", key.publisher, error) ||
               !required_string(item, "publicKeyPem", key.public_key_pem, error) ||
               !item.contains("roles") || !item.at("roles").is_array()) return std::nullopt;
            for(const auto& role_value : item.at("roles")) {
                if(!role_value.is_string()) { fail(error, "invalid trust role"); return std::nullopt; }
                auto role = skill_trust_role_from_name(role_value.get<std::string>());
                if(!role) { fail(error, "unknown trust role"); return std::nullopt; }
                key.roles.insert(*role);
            }
            key.source_prefixes = item.value("sourcePrefixes", std::vector<std::string>{});
            key.not_before = item.value("notBefore", std::int64_t{0});
            key.not_after = item.value("notAfter", std::int64_t{0});
            if(key.roles.empty()) { fail(error, "trusted key has no roles"); return std::nullopt; }
            out.keys.push_back(std::move(key));
        }
        out.revoked_publishers = value.value("revokedPublishers", std::set<std::string>{});
        out.revoked_keys = value.value("revokedKeys", std::set<std::string>{});
        out.revoked_packages = value.value("revokedPackages", std::set<std::string>{});
    } catch(const std::exception& ex) {
        fail(error, ex.what());
        return std::nullopt;
    }
    return out;
}

json SkillRegistryIndex::to_json() const {
    json entries = json::array();
    for(const auto& artifact : artifacts) {
        entries.push_back({{"packageId", artifact.package_id}, {"version", artifact.version},
                           {"digest", artifact.digest}, {"size", artifact.size},
                           {"mirrors", artifact.mirrors}, {"signatureUri", artifact.signature_uri}});
    }
    return {{"apiVersion", api_version}, {"registryId", registry_id},
            {"generation", generation}, {"artifacts", entries}};
}

std::optional<SkillRegistryIndex> SkillRegistryIndex::from_json(const json& value,
                                                                std::string* error) {
    SkillRegistryIndex out;
    if(!value.is_object() || !required_string(value, "apiVersion", out.api_version, error) ||
       out.api_version != "agent.taskflow/skill-registry/v1" ||
       !required_string(value, "registryId", out.registry_id, error) ||
       !value.contains("generation") || !value.at("generation").is_number_unsigned() ||
       !value.contains("artifacts") || !value.at("artifacts").is_array()) return std::nullopt;
    out.generation = value.at("generation").get<std::uint64_t>();
    for(const auto& item : value.at("artifacts")) {
        SkillRegistryArtifact artifact;
        if(!required_string(item, "packageId", artifact.package_id, error) ||
           !required_string(item, "version", artifact.version, error) ||
           !required_digest(item, "digest", artifact.digest, error) ||
           !item.contains("size") || !item.at("size").is_number_unsigned()) return std::nullopt;
        try {
            artifact.size = item.at("size").get<std::uint64_t>();
            artifact.mirrors = item.value("mirrors", std::vector<std::string>{});
            artifact.signature_uri = item.value("signatureUri", std::string{});
        } catch(const std::exception& ex) { fail(error, ex.what()); return std::nullopt; }
        if(artifact.mirrors.empty() || artifact.signature_uri.empty()) {
            fail(error, "registry artifact requires mirrors and signatureUri");
            return std::nullopt;
        }
        out.artifacts.push_back(std::move(artifact));
    }
    return out;
}

} // namespace agent_framework
