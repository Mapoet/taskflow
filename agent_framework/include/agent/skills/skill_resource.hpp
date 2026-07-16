#ifndef AGENT_SKILL_RESOURCE_HPP
#define AGENT_SKILL_RESOURCE_HPP

#include <agent/skills/skill_permissions.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace agent_framework {

enum class SkillResourceType {
    Script, Cli, Reference, Tool, Mcp, Template, Schema, Prompt,
    Workflow, Config, Asset, Model, Test, Unknown
};

enum class SkillResourceReadMode { Auto, Text, Binary, Stream, MemoryMap, Unknown };
enum class SkillCachePolicy { NoStore, OnDemand, Pin, Unknown };

struct SkillCitationMetadata {
    std::string title;
    std::vector<std::string> authors;
    std::string published;
    std::string url;
    std::string locator;
};

struct SkillReferenceIndexConfig {
    std::string kind;
};

struct SkillModelRequirements {
    std::vector<std::string> devices;
    std::vector<std::string> precisions;
    std::uint64_t min_memory_bytes = 0;
};

struct SkillResourceDescriptor {
    std::string id;
    SkillResourceType kind = SkillResourceType::Unknown;
    std::string path;
    std::string media_type;
    std::string sha256;
    std::optional<std::uint64_t> declared_size;
    std::optional<std::size_t> size_limit;
    bool optional = false;
    bool executable = false;
    std::string input_schema;
    std::string output_schema;
    std::string runtime;
    std::string license;
    std::string source_uri;
    SkillResourceReadMode read_mode = SkillResourceReadMode::Auto;
    SkillCachePolicy cache_policy = SkillCachePolicy::NoStore;
    std::optional<SkillCitationMetadata> citation;
    std::optional<SkillReferenceIndexConfig> index;
    std::optional<SkillModelRequirements> model_requirements;
    SkillPermissionSet permissions;
    std::vector<std::string> depends_on;
};

std::string to_string(SkillResourceType kind);
SkillResourceType skill_resource_type_from_string(const std::string& value);
std::string to_string(SkillResourceReadMode mode);
SkillResourceReadMode skill_resource_read_mode_from_string(const std::string& value);
std::string to_string(SkillCachePolicy policy);
SkillCachePolicy skill_cache_policy_from_string(const std::string& value);

} // namespace agent_framework

#endif
