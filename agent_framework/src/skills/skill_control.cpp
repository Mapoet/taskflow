#include <agent/skills/skill_control.hpp>

namespace agent_framework {

SkillControlResult dispatch_skill_control(const ControlAction& action,
                                          const std::shared_ptr<SkillManager>& manager) {
    SkillControlResult result;
    if (action.command.rfind("skills.", 0) != 0) return result;
    result.handled = true;
    if (!manager) {
        result.value = {{"ok", false}, {"code", "skills_unavailable"},
                        {"message", "No Skill roots are configured"}};
    } else if (action.command == "skills.list") result.value = manager->list();
    else if (action.command == "skills.status") result.value = manager->status();
    else if (action.command == "skills.reload") result.value = manager->reload();
    else if (action.command == "skills.validate") result.value = manager->validate(action.args.value("id", ""));
    else if (action.command == "skills.create")
        result.value = manager->create(action.args.value("id", ""), action.args.value("description", ""));
    else if (action.command == "skills.activate") result.value = manager->activate(action.args.value("id", ""));
    else if (action.command == "skills.deactivate") result.value = manager->deactivate();
    else result.value = {{"ok", false}, {"code", "skill_command_unknown"}};
    result.ok = result.value.value("ok", false);
    result.text = result.value.dump(2);
    return result;
}

} // namespace agent_framework
