#ifndef AGENT_SKILL_RESOURCE_HPP
#define AGENT_SKILL_RESOURCE_HPP

#include <cstddef>
#include <optional>
#include <string>

namespace agent_framework {

enum class SkillResourceType {
    Script, Cli, Reference, Tool, Mcp, Template, Schema, Prompt,
    Workflow, Config, Asset, Model, Test, Unknown
};

struct SkillResourceDescriptor {
    std::string id;
    SkillResourceType kind = SkillResourceType::Unknown;
    std::string path;
    std::string media_type;
    std::string sha256;
    std::optional<std::size_t> size_limit;
    bool optional = false;
    bool executable = false;
    std::string input_schema;
    std::string output_schema;
    std::string runtime;
    std::string cache_policy;
};

std::string to_string(SkillResourceType kind);
SkillResourceType skill_resource_type_from_string(const std::string& value);

} // namespace agent_framework

#endif
