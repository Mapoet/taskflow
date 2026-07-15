#ifndef __AGENT_SKILL_COMMAND_H__
#define __AGENT_SKILL_COMMAND_H__

#include <agent/skill_loader.hpp>
#include <agent/skill_registry.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace agent_framework {

enum class SkillCliExit : int {
    Success = 0,
    ContractFailed = 2,
    NotFound = 3,
    OperationFailed = 4,
    IntegrityFailed = 5,
    DependencyUnavailable = 6,
    Usage = 64,
    Internal = 70,
};

struct SkillCommandResponse {
    SkillCliExit exit = SkillCliExit::Success;
    std::string command;
    nlohmann::json data = nlohmann::json::object();
    std::vector<SkillDiagnostic> diagnostics;
    nlohmann::json error = nullptr;
    std::optional<std::string> raw_output;

    bool ok() const noexcept;
    nlohmann::json to_json() const;
};

struct SkillCliArguments {
    std::filesystem::path root;
    std::filesystem::path store;
    std::string format = "json";
    std::string command;
    std::vector<std::string> operands;
    bool help = false;
};

struct SkillCliParseResult {
    std::optional<SkillCliArguments> arguments;
    SkillCommandResponse response;

    explicit operator bool() const noexcept { return arguments.has_value(); }
};

SkillCliParseResult parse_skill_cli_arguments(const std::vector<std::string>& tokens);
std::string skillctl_usage();

class SkillCommandService {
public:
    explicit SkillCommandService(std::shared_ptr<SkillRegistry> registry);

    SkillCommandResponse list() const;
    SkillCommandResponse validate() const;
    SkillCommandResponse show(const std::string& skill_id) const;
    SkillCommandResponse inspect(const std::string& skill_id, bool resolved) const;
    SkillCommandResponse read(const std::string& skill_id, SkillResourceKind kind,
                              const std::string& relative_path, std::size_t max_bytes,
                              bool raw) const;

private:
    std::shared_ptr<SkillRegistry> registry_;
    SkillLoader loader_;
};

std::optional<SkillResourceKind> parse_skill_resource_kind(const std::string& value);

} // namespace agent_framework

#endif // __AGENT_SKILL_COMMAND_H__
