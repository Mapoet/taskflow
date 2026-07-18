#ifndef AGENT_RESOURCE_URI_HPP
#define AGENT_RESOURCE_URI_HPP

#include <string>
#include <string_view>

namespace agent_framework {

enum class ResourceScheme { Workspace, Skill, SkillCache, Mcp };

class ResourceUri {
public:
    static ResourceUri parse(std::string_view value);

    ResourceScheme scheme() const noexcept { return scheme_; }
    const std::string& authority() const noexcept { return authority_; }
    const std::string& path() const noexcept { return path_; }
    std::string str() const;

private:
    ResourceScheme scheme_ = ResourceScheme::Workspace;
    std::string authority_;
    std::string path_;
};

const char* resource_scheme_name(ResourceScheme scheme) noexcept;

} // namespace agent_framework
#endif
