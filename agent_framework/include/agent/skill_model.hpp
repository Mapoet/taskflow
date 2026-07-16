#ifndef AGENT_SKILL_MODEL_HPP
#define AGENT_SKILL_MODEL_HPP

#include "skill_resource_cache.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace agent_framework {

struct SkillModelHostCapabilities {
    std::vector<std::string> runtimes;
    std::vector<std::string> devices;
    std::vector<std::string> precisions;
    std::uint64_t available_memory_bytes = 0;
    std::uint64_t max_readonly_bytes = 256U * 1024U * 1024U;
    bool mmap_supported = false;
};

struct SkillModelAdmissionResult {
    bool ok = false;
    bool compatible = false;
    std::vector<std::string> codes;
    nlohmann::json diagnostics = nlohmann::json::array();
};

struct SkillModelReadOnlyHandle {
    SkillResourceHandle resource;
    SkillCacheObject cache_object;
    std::shared_ptr<SkillCacheLease> cache_lease;
};

struct SkillModelOpenResult {
    bool ok = false;
    nlohmann::json error = nlohmann::json::object();
    SkillModelAdmissionResult admission;
    std::optional<SkillModelReadOnlyHandle> handle;
};

class SkillModelService {
public:
    explicit SkillModelService(std::shared_ptr<SkillResourceCache> cache)
        : cache_(std::move(cache)) {}

    SkillModelAdmissionResult check(const SkillResourceHandle& handle,
                                    const SkillModelHostCapabilities& host) const;
    SkillModelOpenResult open(const SkillResourceHandle& handle,
                              const SkillModelHostCapabilities& host) const;

private:
    std::shared_ptr<SkillResourceCache> cache_;
};

} // namespace agent_framework

#endif
