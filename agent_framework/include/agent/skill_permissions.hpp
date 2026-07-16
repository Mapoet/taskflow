#ifndef AGENT_SKILL_PERMISSIONS_HPP
#define AGENT_SKILL_PERMISSIONS_HPP

#include <string>
#include <vector>

namespace agent_framework {

struct SkillPermissionSet {
    std::vector<std::string> tools;
    std::vector<std::string> network;
    std::vector<std::string> environment;
    std::vector<std::string> filesystem_read;
    std::vector<std::string> filesystem_write;
    std::vector<std::string> secrets;
};

bool skill_permissions_empty(const SkillPermissionSet& permissions) noexcept;
SkillPermissionSet skill_permissions_effective(const SkillPermissionSet& manifest,
                                               const SkillPermissionSet& resource);

} // namespace agent_framework

#endif
