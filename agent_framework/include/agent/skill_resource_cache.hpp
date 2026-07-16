#ifndef AGENT_SKILL_RESOURCE_CACHE_HPP
#define AGENT_SKILL_RESOURCE_CACHE_HPP

#include "skill_resource_access.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace agent_framework {

struct SkillCacheLimits {
    std::uint64_t max_object_bytes = 256U * 1024U * 1024U;
    std::uint64_t max_total_bytes = 1024U * 1024U * 1024U;
};

struct SkillCacheObject {
    std::string digest;
    std::filesystem::path path;
    std::filesystem::path metadata_path;
    std::uint64_t size = 0;
    std::string media_type;
    std::string source_package_digest;
    std::string source_resource_id;
    std::uint64_t last_access = 0;
    bool pinned = false;
    bool leased = false;
};

class SkillCacheLease {
public:
    const SkillCacheObject& object() const noexcept { return object_; }

private:
    friend class SkillResourceCache;
    SkillCacheObject object_;
    std::shared_ptr<const void> token_;
};

struct SkillCacheResult {
    bool ok = false;
    nlohmann::json error = nlohmann::json::object();
    std::optional<SkillCacheObject> object;
    std::shared_ptr<SkillCacheLease> lease;
};

struct SkillCacheReport {
    bool ok = false;
    nlohmann::json error = nlohmann::json::object();
    std::uint64_t total_bytes = 0;
    std::uint64_t pinned_bytes = 0;
    std::size_t object_count = 0;
    std::size_t leased_objects = 0;
    std::vector<SkillCacheObject> entries;
};

class SkillResourceCache {
public:
    explicit SkillResourceCache(std::filesystem::path root,
                                SkillCacheLimits limits = {});

    SkillCacheResult acquire(const SkillResourceHandle& handle);
    /** Apply the descriptor policy: no-store, on-demand, or pin. */
    SkillCacheResult acquire_policy(const SkillResourceHandle& handle);
    SkillCacheReport inspect() const;
    SkillCacheResult pin(const std::string& digest);
    SkillCacheResult unpin(const std::string& digest);
    SkillCacheResult collect();
    SkillCacheResult verify();
    const std::filesystem::path& root() const noexcept { return root_; }
    const SkillCacheLimits& limits() const noexcept { return limits_; }

private:
    SkillCacheResult acquire_locked(const SkillResourceHandle& handle);
    SkillCacheReport inspect_locked() const;
    SkillCacheResult set_pin_locked(const std::string& digest, bool pinned);
    SkillCacheResult collect_locked(std::uint64_t required_bytes = 0,
                                    const std::string& protected_digest = {});
    std::filesystem::path root_;
    SkillCacheLimits limits_;
    mutable std::mutex mutex_;
    std::map<std::string, std::weak_ptr<const void>> leases_;
};

} // namespace agent_framework

#endif
