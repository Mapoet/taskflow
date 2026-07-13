#ifndef AGENT_SKILL_POLICY_HPP
#define AGENT_SKILL_POLICY_HPP

#include "skill_manifest.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace agent_framework {

enum class SkillPermissionKind {
    Tool,
    Network,
    Environment,
    FilesystemRead,
    FilesystemWrite,
    Secret
};

struct SkillPermissionGrant {
    std::vector<std::string> tools;
    std::vector<std::string> network;
    std::vector<std::string> environment;
    std::vector<std::string> filesystem_read;
    std::vector<std::string> filesystem_write;
    std::vector<std::string> secrets;
};

struct SkillPolicyDecision {
    bool allowed = false;
    SkillPermissionKind kind = SkillPermissionKind::Tool;
    std::string action;
    std::string target;
    std::string reason;
};

class SkillPolicyEngine {
public:
    SkillPolicyEngine(SkillPermissionSet requested, SkillPermissionGrant granted,
                      std::filesystem::path package_root = {});

    SkillPolicyDecision authorize_tool(std::string_view tool_name) const;
    SkillPolicyDecision authorize_network(std::string_view url_or_origin) const;
    SkillPolicyDecision authorize_environment(std::string_view variable) const;
    SkillPolicyDecision authorize_filesystem(const std::filesystem::path& path, bool write) const;
    SkillPolicyDecision authorize_secret(std::string_view reference) const;

    const SkillPermissionSet& requested() const noexcept { return requested_; }
    const SkillPermissionGrant& granted() const noexcept { return granted_; }

private:
    SkillPermissionSet requested_;
    SkillPermissionGrant granted_;
    std::filesystem::path package_root_;
};

const char* skill_permission_kind_cstr(SkillPermissionKind kind) noexcept;

} // namespace agent_framework

#endif
