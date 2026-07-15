#ifndef AGENT_SKILL_TEST_RUNNER_HPP
#define AGENT_SKILL_TEST_RUNNER_HPP

#include <agent/skill_types.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace agent_framework {

struct SkillTestTarget {
    std::string kind;
    std::string resource;
};

struct SkillTestDescriptor {
    std::string name;
    SkillTestTarget target;
    nlohmann::json input = nlohmann::json::object();
    nlohmann::json mocks = nlohmann::json::object();
    nlohmann::json expect = nlohmann::json::object();
    std::filesystem::path source;
};

struct SkillTestParseResult {
    std::optional<SkillTestDescriptor> descriptor;
    std::vector<SkillDiagnostic> diagnostics;
};

SkillTestParseResult parse_skill_test_descriptor(
    const nlohmann::json& value, const std::filesystem::path& source = {});
SkillTestParseResult parse_skill_test_file(const std::filesystem::path& path);

} // namespace agent_framework

#endif
