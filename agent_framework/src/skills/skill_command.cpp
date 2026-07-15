#include <agent/skill_command.hpp>

namespace agent_framework {

namespace {

nlohmann::json diagnostic_to_json(const SkillDiagnostic& diagnostic) {
    return {
        {"severity", diagnostic.severity == SkillDiagnosticSeverity::Error ? "error" : "warning"},
        {"code", diagnostic.code},
        {"path", diagnostic.path.generic_string()},
        {"location", diagnostic.location},
        {"message", diagnostic.message},
        {"suggestion", diagnostic.suggestion},
    };
}

} // namespace

bool SkillCommandResponse::ok() const noexcept {
    return exit == SkillCliExit::Success;
}

nlohmann::json SkillCommandResponse::to_json() const {
    auto serialized_diagnostics = nlohmann::json::array();
    for(const auto& diagnostic : diagnostics) {
        serialized_diagnostics.push_back(diagnostic_to_json(diagnostic));
    }

    return {
        {"apiVersion", "agent.taskflow/skillctl-output/v1"},
        {"command", command},
        {"ok", ok()},
        {"data", data},
        {"diagnostics", std::move(serialized_diagnostics)},
        {"error", error},
    };
}

} // namespace agent_framework
