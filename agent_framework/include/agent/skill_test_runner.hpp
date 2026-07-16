#ifndef AGENT_SKILL_TEST_RUNNER_HPP
#define AGENT_SKILL_TEST_RUNNER_HPP

#include <agent/skill_registry.hpp>
#include <agent/task_state_machine.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
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

struct SkillTestRunOptions {
    std::string filter;
    std::size_t jobs = 1;
    std::chrono::milliseconds timeout{5000};
    std::shared_ptr<TaskControl> control;
    std::function<void(const std::string&, bool)> case_state_observer;
    bool exact_filter = false;
};

struct SkillTestCaseResult {
    std::string name;
    bool passed = false;
    nlohmann::json output = nullptr;
    nlohmann::json error = nlohmann::json::object();
    std::vector<std::string> events;
    std::string stdout_text;
    std::string stderr_text;
    std::optional<int> exit_code;
    std::map<std::string, std::string> resource_digests;
    std::uint64_t duration_ms = 0;

    nlohmann::json to_json() const;
};

struct SkillTestSuiteResult {
    bool ok = false;
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::vector<SkillTestCaseResult> cases;
    nlohmann::json error = nullptr;

    nlohmann::json to_json() const;
};

class SkillTestRunner {
public:
    explicit SkillTestRunner(std::shared_ptr<SkillRegistry> registry)
        : registry_(std::move(registry)) {}

    SkillTestSuiteResult run(const std::string& skill_id,
                             const SkillTestRunOptions& options = {}) const;

private:
    std::shared_ptr<SkillRegistry> registry_;
};

} // namespace agent_framework

#endif
