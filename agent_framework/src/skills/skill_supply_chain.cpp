#include "agent/skill_supply_chain.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#endif

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

void append_field(std::string& output, const std::string& value) {
    const auto size = static_cast<std::uint32_t>(value.size());
    output.push_back(static_cast<char>((size >> 24) & 0xff));
    output.push_back(static_cast<char>((size >> 16) & 0xff));
    output.push_back(static_cast<char>((size >> 8) & 0xff));
    output.push_back(static_cast<char>(size & 0xff));
    output.append(value);
}

#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
std::optional<std::string> public_der(EVP_PKEY* key, std::string* error) {
    const auto length = i2d_PUBKEY(key, nullptr);
    if(length <= 0) { fail(error, "cannot encode public key"); return std::nullopt; }
    std::string der(static_cast<std::size_t>(length), '\0');
    auto* cursor = reinterpret_cast<unsigned char*>(der.data());
    if(i2d_PUBKEY(key, &cursor) != length) { fail(error, "cannot encode public key"); return std::nullopt; }
    return der;
}

EVP_PKEY* read_public_key(const std::string& pem, std::string* error) {
    auto* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if(!bio) { fail(error, "public key buffer allocation failed"); return nullptr; }
    auto* key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if(!key || EVP_PKEY_id(key) != EVP_PKEY_ED25519) {
        if(key) EVP_PKEY_free(key);
        fail(error, "public key is not Ed25519 PEM");
        return nullptr;
    }
    return key;
}

EVP_PKEY* read_private_key(const std::string& pem, std::string* error) {
    auto* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if(!bio) { fail(error, "private key buffer allocation failed"); return nullptr; }
    auto* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if(!key || EVP_PKEY_id(key) != EVP_PKEY_ED25519) {
        if(key) EVP_PKEY_free(key);
        fail(error, "private key is not Ed25519 PEM");
        return nullptr;
    }
    return key;
}

std::string base64_encode(const unsigned char* bytes, std::size_t size) {
    std::string output(4 * ((size + 2) / 3), '\0');
    const auto written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()), bytes,
                                         static_cast<int>(size));
    output.resize(static_cast<std::size_t>(written));
    return output;
}

std::optional<std::string> base64_decode(const std::string& value, std::string* error) {
    if(value.empty() || value.size() > 1024 || value.size() % 4 != 0 ||
       !std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return std::isalnum(c) || c == '+' || c == '/' || c == '=';
       })) {
        fail(error, "invalid base64 signature");
        return std::nullopt;
    }
    std::string output(3 * (value.size() / 4), '\0');
    const auto decoded = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(output.data()),
        reinterpret_cast<const unsigned char*>(value.data()), static_cast<int>(value.size()));
    if(decoded < 0) { fail(error, "invalid base64 signature"); return std::nullopt; }
    std::size_t padding = 0;
    if(!value.empty() && value.back() == '=') ++padding;
    if(value.size() > 1 && value[value.size() - 2] == '=') ++padding;
    output.resize(static_cast<std::size_t>(decoded) - padding);
    return output;
}
#endif

} // namespace

std::string skill_trust_role_name(SkillTrustRole role) {
    return role == SkillTrustRole::Package ? "package" : "registry";
}

std::optional<SkillTrustRole> skill_trust_role_from_name(const std::string& value) {
    if(value == "package") return SkillTrustRole::Package;
    if(value == "registry") return SkillTrustRole::Registry;
    return std::nullopt;
}

std::optional<std::string> skill_sha256_bytes(const std::string& value, std::string* error) {
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    auto* context = EVP_MD_CTX_new();
    if(!context) { fail(error, "SHA-256 context allocation failed"); return std::nullopt; }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    const bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
                    EVP_DigestUpdate(context, value.data(), value.size()) == 1 &&
                    EVP_DigestFinal_ex(context, digest, &size) == 1;
    EVP_MD_CTX_free(context);
    if(!ok) { fail(error, "SHA-256 operation failed"); return std::nullopt; }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for(unsigned int i = 0; i < size; ++i) output << std::setw(2) << static_cast<unsigned int>(digest[i]);
    return output.str();
#else
    (void)value;
    fail(error, "SHA-256 unavailable because OpenSSL was not found");
    return std::nullopt;
#endif
}

std::string skill_signature_preimage(const SkillSignatureEnvelope& envelope) {
    static constexpr char magic[] = "agent.taskflow/skill-signature/v1";
    std::string output(magic, sizeof(magic));
    append_field(output, envelope.algorithm);
    append_field(output, envelope.subject_kind);
    append_field(output, envelope.subject_digest);
    append_field(output, envelope.key_id);
    append_field(output, envelope.publisher);
    append_field(output, envelope.source_uri);
    append_field(output, envelope.sbom_digest);
    append_field(output, envelope.provenance_digest);
    return output;
}

std::optional<std::string> skill_public_key_id(const std::string& public_key_pem,
                                               std::string* error) {
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    auto* key = read_public_key(public_key_pem, error);
    if(!key) return std::nullopt;
    auto der = public_der(key, error);
    EVP_PKEY_free(key);
    return der ? skill_sha256_bytes(*der, error) : std::nullopt;
#else
    (void)public_key_pem;
    fail(error, "Ed25519 unavailable because OpenSSL was not found");
    return std::nullopt;
#endif
}

SkillSignatureResult sign_skill_subject(SkillSignatureEnvelope envelope,
                                        const std::string& private_key_pem) {
    SkillSignatureResult result;
    result.envelope = std::move(envelope);
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    std::string error;
    auto* key = read_private_key(private_key_pem, &error);
    if(!key) { result.error = error; return result; }
    auto der = public_der(key, &error);
    auto key_id = der ? skill_sha256_bytes(*der, &error) : std::nullopt;
    if(!key_id) { EVP_PKEY_free(key); result.error = error; return result; }
    result.envelope.key_id = *key_id;
    result.envelope.signature.clear();
    std::string validation_error;
    auto unsigned_json = result.envelope.to_json();
    unsigned_json["signature"] = "pending";
    if(!SkillSignatureEnvelope::from_json(unsigned_json, &validation_error)) {
        EVP_PKEY_free(key); result.error = validation_error; return result;
    }
    const auto preimage = skill_signature_preimage(result.envelope);
    std::array<unsigned char, 64> signature{};
    std::size_t signature_size = signature.size();
    auto* context = EVP_MD_CTX_new();
    const bool ok = context && EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) == 1 &&
                    EVP_DigestSign(context, signature.data(), &signature_size,
                                   reinterpret_cast<const unsigned char*>(preimage.data()),
                                   preimage.size()) == 1 && signature_size == signature.size();
    if(context) EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    if(!ok) { result.error = std::string(kSkillSignatureInvalid) + ": Ed25519 signing failed"; return result; }
    result.envelope.signature = base64_encode(signature.data(), signature_size);
    result.ok = true;
#else
    (void)private_key_pem;
    result.error = std::string(kSkillSignatureInvalid) + ": Ed25519 unavailable because OpenSSL was not found";
#endif
    return result;
}

SkillSignatureResult verify_skill_signature(const SkillSignatureEnvelope& envelope,
                                            const SkillTrustStore& trust,
                                            SkillTrustRole required_role,
                                            std::int64_t now) {
    SkillSignatureResult result;
    result.envelope = envelope;
    std::string parse_error;
    if(!SkillSignatureEnvelope::from_json(envelope.to_json(), &parse_error)) {
        result.error = parse_error;
        return result;
    }
    const auto expected_kind = required_role == SkillTrustRole::Package ? "package" : "registry";
    if(envelope.subject_kind != expected_kind || trust.revoked_publishers.count(envelope.publisher) ||
       trust.revoked_keys.count(envelope.key_id) ||
       (required_role == SkillTrustRole::Package && trust.revoked_packages.count(envelope.subject_digest))) {
        result.error = std::string(kSkillTrustDenied) + ": role mismatch or revoked identity";
        return result;
    }
    const SkillTrustedKey* trusted = nullptr;
    for(const auto& candidate : trust.keys) {
        if(candidate.key_id == envelope.key_id && candidate.publisher == envelope.publisher) {
            trusted = &candidate;
            break;
        }
    }
    if(!trusted || !trusted->roles.count(required_role) ||
       (trusted->not_before && now < trusted->not_before) ||
       (trusted->not_after && now > trusted->not_after)) {
        result.error = std::string(kSkillTrustDenied) + ": key is unknown, out of role, or expired";
        return result;
    }
    if(trusted->source_prefixes.empty() ||
       std::none_of(trusted->source_prefixes.begin(), trusted->source_prefixes.end(),
                    [&](const std::string& prefix) { return !prefix.empty() && envelope.source_uri.rfind(prefix, 0) == 0; })) {
        result.error = std::string(kSkillTrustDenied) + ": source is outside key scope";
        return result;
    }
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    auto* key = read_public_key(trusted->public_key_pem, &parse_error);
    if(!key) { result.error = parse_error; return result; }
    auto der = public_der(key, &parse_error);
    auto actual_key_id = der ? skill_sha256_bytes(*der, &parse_error) : std::nullopt;
    auto signature = base64_decode(envelope.signature, &parse_error);
    if(!actual_key_id || *actual_key_id != envelope.key_id || !signature || signature->size() != 64) {
        EVP_PKEY_free(key);
        result.error = parse_error.empty() ? std::string(kSkillTrustDenied) + ": key identity mismatch" : parse_error;
        return result;
    }
    const auto preimage = skill_signature_preimage(envelope);
    auto* context = EVP_MD_CTX_new();
    const bool ok = context && EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1 &&
                    EVP_DigestVerify(context,
                        reinterpret_cast<const unsigned char*>(signature->data()), signature->size(),
                        reinterpret_cast<const unsigned char*>(preimage.data()), preimage.size()) == 1;
    if(context) EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    if(!ok) { result.error = std::string(kSkillSignatureInvalid) + ": signature verification failed"; return result; }
    result.ok = true;
#else
    result.error = std::string(kSkillSignatureInvalid) + ": Ed25519 unavailable because OpenSSL was not found";
#endif
    return result;
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
