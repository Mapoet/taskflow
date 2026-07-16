#ifndef AGENT_SKILL_REMOTE_REGISTRY_HPP
#define AGENT_SKILL_REMOTE_REGISTRY_HPP

#include "skill_supply_chain.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace agent_framework {

struct SkillRegistryFetchResult {
    bool ok = false;
    std::string error;
    std::string bytes;
};

class SkillRegistryTransport {
public:
    virtual ~SkillRegistryTransport() = default;
    virtual SkillRegistryFetchResult fetch(const std::string& uri,
                                           std::uint64_t max_bytes) = 0;
};

class SkillMemoryRegistryTransport final : public SkillRegistryTransport {
public:
    std::map<std::string, std::string> responses;
    std::vector<std::string> requests;
    SkillRegistryFetchResult fetch(const std::string& uri,
                                   std::uint64_t max_bytes) override;
private:
    std::mutex mutex_;
};

class SkillCurlRegistryTransport final : public SkillRegistryTransport {
public:
    SkillRegistryFetchResult fetch(const std::string& uri,
                                   std::uint64_t max_bytes) override;
};

struct SkillRemoteRegistryResult {
    bool ok = false;
    std::string error;
    std::string index_digest;
    SkillRegistryIndex index;
    SkillSignatureEnvelope signature;
};

struct SkillPinnedPackageResult {
    bool ok = false;
    std::string error;
    std::string digest;
    std::string selected_uri;
    SkillSignatureEnvelope signature;
};

class SkillRemoteRegistryClient {
public:
    static constexpr std::uint64_t kMaxIndexBytes = 4ull * 1024 * 1024;
    static constexpr std::uint64_t kMaxSignatureBytes = 64ull * 1024;
    static constexpr std::uint64_t kMaxPackageBytes = 512ull * 1024 * 1024;

    SkillRemoteRegistryClient(SkillTrustStore trust,
                              std::shared_ptr<SkillRegistryTransport> transport);

    SkillRemoteRegistryResult sync(const std::string& index_uri,
                                   const std::string& signature_uri,
                                   std::int64_t now) const;
    std::optional<SkillRegistryArtifact> resolve(const SkillRegistryIndex& index,
                                                 const std::string& package_id,
                                                 const std::string& version,
                                                 std::string* error = nullptr) const;
    SkillPinnedPackageResult fetch_pinned(const SkillRegistryArtifact& artifact,
                                          const std::filesystem::path& destination,
                                          std::int64_t now) const;
    SkillPinnedPackageResult verify_offline(const std::filesystem::path& archive,
                                            const std::filesystem::path& signature,
                                            const std::string& expected_digest,
                                            std::int64_t now) const;

private:
    SkillTrustStore trust_;
    std::shared_ptr<SkillRegistryTransport> transport_;
};

} // namespace agent_framework

#endif
