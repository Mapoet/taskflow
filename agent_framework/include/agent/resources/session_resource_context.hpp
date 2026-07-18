#ifndef AGENT_SESSION_RESOURCE_CONTEXT_HPP
#define AGENT_SESSION_RESOURCE_CONTEXT_HPP

#include <agent/resources/resource_uri.hpp>
#include <agent/skills/skill_registry.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace agent_framework {

struct ResourceDomainStatus {
    std::string uri_prefix;
    std::filesystem::path root;
    bool writable = false;
};

class SessionResourceContext {
public:
    SessionResourceContext(std::filesystem::path workspace_root,
                           std::vector<std::filesystem::path> skill_roots,
                           std::filesystem::path authoring_root,
                           std::filesystem::path cache_root,
                           SkillRegistrySnapshot snapshot = {});

    std::filesystem::path resolve_local(const ResourceUri& uri) const;
    bool mcp_allowed(std::string_view service) const;
    void set_allowed_mcp_services(std::unordered_set<std::string> services);
    std::vector<ResourceDomainStatus> domains() const;

    const std::filesystem::path& workspace_root() const noexcept { return workspace_root_; }
    const std::vector<std::filesystem::path>& skill_roots() const noexcept { return skill_roots_; }
    const std::filesystem::path& authoring_root() const noexcept { return authoring_root_; }
    const std::filesystem::path& cache_root() const noexcept { return cache_root_; }
    std::uint64_t registry_generation() const noexcept { return snapshot_.generation(); }
    const SkillRegistrySnapshot& skill_snapshot() const noexcept { return snapshot_; }

private:
    static std::filesystem::path jailed(const std::filesystem::path& root,
                                        const std::filesystem::path& relative);
    std::filesystem::path workspace_root_;
    std::vector<std::filesystem::path> skill_roots_;
    std::filesystem::path authoring_root_;
    std::filesystem::path cache_root_;
    SkillRegistrySnapshot snapshot_;
    std::unordered_set<std::string> allowed_mcp_services_;
};

} // namespace agent_framework
#endif
