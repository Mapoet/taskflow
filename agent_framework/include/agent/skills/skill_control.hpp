#ifndef AGENT_SKILL_CONTROL_HPP
#define AGENT_SKILL_CONTROL_HPP

#include <agent/agent/user_input_types.hpp>
#include <agent/skills/skill_manager.hpp>

#include <memory>
#include <string>

namespace agent_framework {

struct SkillControlResult {
    bool handled = false;
    bool ok = false;
    std::string text;
    nlohmann::json value = nlohmann::json::object();
};

SkillControlResult dispatch_skill_control(const ControlAction& action,
                                          const std::shared_ptr<SkillManager>& manager);

} // namespace agent_framework
#endif
