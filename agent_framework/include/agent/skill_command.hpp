#ifndef __AGENT_SKILL_COMMAND_H__
#define __AGENT_SKILL_COMMAND_H__

#include <agent/skill_types.hpp>

#include <nlohmann/json.hpp>

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

    bool ok() const noexcept;
    nlohmann::json to_json() const;
};

} // namespace agent_framework

#endif // __AGENT_SKILL_COMMAND_H__
