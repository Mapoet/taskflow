#ifndef AGENT_SKILL_MANIFEST_HPP
#define AGENT_SKILL_MANIFEST_HPP

#include "skill_resource.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace agent_framework {

struct SkillDependency {
    std::string name;
    std::string version;
    bool optional = false;
};

struct SkillPermissionSet {
    std::vector<std::string> tools;
    std::vector<std::string> network;
    std::vector<std::string> environment;
    std::vector<std::string> filesystem_read;
    std::vector<std::string> filesystem_write;
    std::vector<std::string> secrets;
};

struct SkillCompatibility {
    std::string agent_framework;
};

struct SkillManifest {
    std::string api_version;
    std::string kind;
    std::string name;
    std::string legacy_id;
    std::string version;
    std::string description;
    std::string license;
    std::vector<std::string> authors;
    std::vector<std::string> tags;
    std::vector<std::string> trigger_keywords;
    bool disable_model_invocation = false;
    SkillCompatibility compatibility;
    std::vector<SkillDependency> dependencies;
    SkillPermissionSet permissions;
    std::vector<SkillResourceDescriptor> resources;
    nlohmann::json extensions = nlohmann::json::object();
    bool legacy_v0 = false;
};

struct SkillManifestIssue {
    bool error = false;
    std::string code;
    std::string location;
    std::string message;
    std::string suggestion;
};

struct SkillManifestParseResult {
    std::optional<SkillManifest> manifest;
    std::vector<SkillManifestIssue> issues;
};

SkillManifestParseResult parse_skill_manifest_yaml(const std::string& yaml);
std::vector<SkillManifestIssue> validate_skill_manifest(
    const SkillManifest& manifest, const std::filesystem::path& package_directory);
nlohmann::json skill_manifest_to_json(const SkillManifest& manifest, bool resolved = true);

} // namespace agent_framework

#endif
