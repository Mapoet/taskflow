#ifndef AGENT_SKILL_CONFIG_HPP
#define AGENT_SKILL_CONFIG_HPP

#include <agent/skills/skill_resource_access.hpp>
#include <agent/skills/skill_runtime.hpp>

#include <map>
#include <memory>
#include <string>

namespace agent_framework {

struct SkillConfigResolveOptions {
    nlohmann::json overrides = nlohmann::json::object();
    /** JSON Pointer -> manifest secret reference. */
    std::map<std::string, std::string> secret_bindings;
};

struct SkillConfigResult {
    bool ok = false;
    nlohmann::json error = nlohmann::json::object();
    nlohmann::json value = nlohmann::json::object();
};

class SkillConfigService {
public:
    explicit SkillConfigService(std::shared_ptr<SkillResourceCache> cache = nullptr)
        : access_(std::move(cache)) {}

    SkillConfigResult resolve(const SkillIndexEntry& entry,
                              std::shared_ptr<const SkillManifest> manifest,
                              const std::string& resource_id,
                              const SkillConfigResolveOptions& options,
                              const SkillInvocationContext& context) const;

private:
    SkillResourceAccess access_;
};

} // namespace agent_framework

#endif
