#include "agent/skill_remote_registry.hpp"

#include <algorithm>
#include <set>

#if defined(HAVE_CURL)
#include <curl/curl.h>
#endif

namespace agent_framework {
namespace {

bool https_uri(const std::string& uri) {
    return uri.rfind("https://", 0) == 0 && uri.size() > 8 &&
           uri.find('@', 8) == std::string::npos && uri.find('#') == std::string::npos;
}

SkillRegistryFetchResult failed(const std::string& detail) {
    return {false, std::string(kSkillRegistryInvalid) + ": " + detail, {}};
}

#if defined(HAVE_CURL)
struct CurlBuffer {
    std::string bytes;
    std::uint64_t limit = 0;
    bool overflow = false;
};

std::size_t curl_write(char* data, std::size_t size, std::size_t count, void* context) {
    auto& buffer = *static_cast<CurlBuffer*>(context);
    const auto bytes = size * count;
    if(bytes > buffer.limit - std::min<std::uint64_t>(buffer.limit, buffer.bytes.size())) {
        buffer.overflow = true;
        return 0;
    }
    buffer.bytes.append(data, bytes);
    return bytes;
}
#endif

} // namespace

SkillRegistryFetchResult SkillMemoryRegistryTransport::fetch(const std::string& uri,
                                                             std::uint64_t max_bytes) {
    requests.push_back(uri);
    const auto found = responses.find(uri);
    if(found == responses.end()) return failed("fixture URI is unavailable");
    if(found->second.size() > max_bytes) return failed("response exceeds limit");
    return {true, {}, found->second};
}

SkillRegistryFetchResult SkillCurlRegistryTransport::fetch(const std::string& uri,
                                                           std::uint64_t max_bytes) {
    if(!https_uri(uri)) return failed("only canonical HTTPS URIs are allowed");
#if defined(HAVE_CURL)
    auto* curl = curl_easy_init();
    if(!curl) return failed("curl initialization failed");
    CurlBuffer buffer{{}, max_bytes, false};
    curl_easy_setopt(curl, CURLOPT_URL, uri.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "taskflow-skill-registry/1");
    const auto code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if(code != CURLE_OK || status < 200 || status >= 300 || buffer.overflow)
        return failed(buffer.overflow ? "response exceeds limit" : "HTTPS request failed");
    return {true, {}, std::move(buffer.bytes)};
#else
    (void)max_bytes;
    return failed("curl transport is unavailable in this build");
#endif
}

SkillRemoteRegistryClient::SkillRemoteRegistryClient(
    SkillTrustStore trust, std::shared_ptr<SkillRegistryTransport> transport)
    : trust_(std::move(trust)), transport_(std::move(transport)) {}

SkillRemoteRegistryResult SkillRemoteRegistryClient::sync(
    const std::string& index_uri, const std::string& signature_uri, std::int64_t now) const {
    SkillRemoteRegistryResult result;
    if(!transport_ || !https_uri(index_uri) || !https_uri(signature_uri)) {
        result.error = std::string(kSkillRegistryInvalid) + ": Registry endpoints must use HTTPS";
        return result;
    }
    auto index_response = transport_->fetch(index_uri, kMaxIndexBytes);
    if(!index_response.ok) { result.error = index_response.error; return result; }
    auto signature_response = transport_->fetch(signature_uri, kMaxSignatureBytes);
    if(!signature_response.ok) { result.error = signature_response.error; return result; }
    try {
        std::string error;
        auto envelope = SkillSignatureEnvelope::from_json(
            nlohmann::json::parse(signature_response.bytes), &error);
        auto digest = skill_sha256_bytes(index_response.bytes, &error);
        if(!envelope || !digest || envelope->subject_kind != "registry" ||
           envelope->subject_digest != *digest || envelope->source_uri != index_uri) {
            result.error = std::string(kSkillRegistryInvalid) + ": signed index identity mismatch";
            return result;
        }
        auto verified = verify_skill_signature(*envelope, trust_, SkillTrustRole::Registry, now);
        if(!verified.ok) { result.error = verified.error; return result; }
        auto index = SkillRegistryIndex::from_json(nlohmann::json::parse(index_response.bytes), &error);
        if(!index) { result.error = error; return result; }
        std::set<std::pair<std::string, std::string>> identities;
        for(const auto& artifact : index->artifacts) {
            if(!identities.emplace(artifact.package_id, artifact.version).second ||
               artifact.size == 0 || artifact.size > kMaxPackageBytes ||
               !https_uri(artifact.signature_uri) ||
               std::any_of(artifact.mirrors.begin(), artifact.mirrors.end(),
                           [](const auto& uri) { return !https_uri(uri); })) {
                result.error = std::string(kSkillRegistryInvalid) +
                               ": index contains duplicate, mutable, insecure, or oversized artifact";
                return result;
            }
        }
        result.ok = true;
        result.index_digest = *digest;
        result.index = std::move(*index);
        result.signature = std::move(*envelope);
    } catch(const std::exception& ex) {
        result.error = std::string(kSkillRegistryInvalid) + ": " + ex.what();
    }
    return result;
}

std::optional<SkillRegistryArtifact> SkillRemoteRegistryClient::resolve(
    const SkillRegistryIndex& index, const std::string& package_id,
    const std::string& version, std::string* error) const {
    for(const auto& artifact : index.artifacts)
        if(artifact.package_id == package_id && artifact.version == version) return artifact;
    if(error) *error = std::string(kSkillRegistryInvalid) + ": immutable package version was not found";
    return std::nullopt;
}

} // namespace agent_framework
