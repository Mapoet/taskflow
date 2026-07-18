#ifndef AGENT_SKILL_MANAGER_HPP
#define AGENT_SKILL_MANAGER_HPP

#include <agent/skills/skill_capability_runtime.hpp>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace agent_framework {

class SkillManager {
public:
    SkillManager(std::shared_ptr<SkillRegistry> registry,
                 std::shared_ptr<SkillLoader> loader,
                 std::shared_ptr<SkillRuntime> runtime,
                 std::filesystem::path authoring_root = {});
    ~SkillManager();

    void attach_toolbus(std::shared_ptr<ToolBus> toolbus,
                        SkillMcpClientFactory mcp_factory = {});
    nlohmann::json status() const;
    nlohmann::json list() const;
    nlohmann::json reload();
    nlohmann::json validate(const std::string& skill_id) const;
    nlohmann::json create(const std::string& skill_id, const std::string& description);
    nlohmann::json activate(const std::string& skill_id,
                            SkillInvocationContext context = {});
    nlohmann::json deactivate();

    std::string active_skill_id() const;
    std::filesystem::path authoring_root() const { return authoring_root_; }

private:
    static bool valid_skill_id(const std::string& id);
    std::shared_ptr<SkillRegistry> registry_;
    std::shared_ptr<SkillLoader> loader_;
    std::shared_ptr<SkillRuntime> runtime_;
    std::shared_ptr<ToolBus> toolbus_;
    std::shared_ptr<SkillCapabilityRuntime> capabilities_;
    std::shared_ptr<SkillCapabilityBinding> binding_;
    std::filesystem::path authoring_root_;
    mutable std::mutex mutex_;
    std::string active_skill_id_;
    nlohmann::json last_reload_ = nlohmann::json::object();
};

} // namespace agent_framework
#endif
