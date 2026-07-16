#ifndef AGENT_SKILL_RESOURCE_ACCESS_HPP
#define AGENT_SKILL_RESOURCE_ACCESS_HPP

#include <agent/skills/skill_registry.hpp>
#include <agent/skills/skill_audit.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace agent_framework {

class TaskControl;
class SkillResourceCache;
class SkillCacheLease;

inline constexpr const char* kSkillResourceNotFound = "skill_resource_not_found";
inline constexpr const char* kSkillResourceCancelled = "skill_resource_cancelled";
inline constexpr const char* kSkillResourceMmapUnavailable = "skill_resource_mmap_unavailable";

struct SkillResourceOpenOptions {
    SkillResourceReadMode mode = SkillResourceReadMode::Auto;
    std::uint64_t offset = 0;
    std::size_t max_bytes = 65536U;
    SkillAuditSink audit_sink;
    SkillAuditIdentity audit_identity;
};

struct SkillResourceHandle {
    std::filesystem::path path;
    SkillResourceDescriptor descriptor;
    std::string package_digest;
    std::string resource_digest;
    std::uint64_t size = 0;
    std::uint64_t view_offset = 0;
    std::uint64_t view_size = 0;
    SkillResourceReadMode mode = SkillResourceReadMode::Auto;
    std::shared_ptr<const void> package_lease;
    std::shared_ptr<SkillCacheLease> cache_lease;
    SkillAuditSink audit_sink;
    SkillAuditIdentity audit_identity;
};

struct SkillResourceResult {
    bool ok = false;
    nlohmann::json error = nlohmann::json::object();
    std::optional<SkillResourceHandle> handle;
};

struct SkillResourceReadResult {
    bool ok = false;
    nlohmann::json error = nlohmann::json::object();
    std::string bytes;
};

struct SkillResourceStreamResult {
    bool ok = false;
    nlohmann::json error = nlohmann::json::object();
    std::uint64_t bytes_read = 0;
};

class SkillMappedResource {
public:
    SkillMappedResource() = default;
    ~SkillMappedResource();
    SkillMappedResource(const SkillMappedResource&) = delete;
    SkillMappedResource& operator=(const SkillMappedResource&) = delete;
    SkillMappedResource(SkillMappedResource&& other) noexcept;
    SkillMappedResource& operator=(SkillMappedResource&& other) noexcept;

    const char* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    std::string_view as_string_view() const noexcept { return {data_, size_}; }

private:
    friend class SkillResourceAccess;
    void reset() noexcept;
    void* mapping_base_ = nullptr;
    std::size_t mapping_size_ = 0;
    const char* data_ = nullptr;
    std::size_t size_ = 0;
    int file_descriptor_ = -1;
};

struct SkillMappedResourceResult {
    bool ok = false;
    nlohmann::json error = nlohmann::json::object();
    std::optional<SkillMappedResource> mapping;
};

using SkillResourceChunkConsumer =
    std::function<bool(std::uint64_t offset, std::string_view chunk)>;

class SkillResourceAccess {
public:
    explicit SkillResourceAccess(std::shared_ptr<SkillResourceCache> cache = nullptr)
        : cache_(std::move(cache)) {}

    SkillResourceResult open_snapshot(
        const SkillIndexEntry& entry, std::shared_ptr<const SkillManifest> manifest,
        const std::string& resource_id,
        const SkillResourceOpenOptions& options = {}) const;

    SkillResourceReadResult read(const SkillResourceHandle& handle) const;
    SkillResourceStreamResult stream(const SkillResourceHandle& handle,
                                     std::size_t chunk_bytes,
                                     const SkillResourceChunkConsumer& consumer,
                                     TaskControl* control = nullptr) const;
    SkillMappedResourceResult map(const SkillResourceHandle& handle) const;

private:
    std::shared_ptr<SkillResourceCache> cache_;
};

} // namespace agent_framework

#endif
