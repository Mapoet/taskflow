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

SkillCliParseResult usage_error(std::string message) {
    SkillCliParseResult result;
    result.response.exit = SkillCliExit::Usage;
    result.response.command = "parse";
    result.response.error = {
        {"code", "skillctl_usage_error"},
        {"message", std::move(message)},
        {"details", {{"usage", skillctl_usage()}}},
    };
    return result;
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

SkillCliParseResult parse_skill_cli_arguments(const std::vector<std::string>& tokens) {
    if(tokens.size() == 1 && (tokens.front() == "--help" || tokens.front() == "-h")) {
        SkillCliParseResult result;
        result.arguments = SkillCliArguments{};
        result.arguments->help = true;
        return result;
    }

    SkillCliArguments parsed;
    std::size_t index = 0;
    while(index < tokens.size() && tokens[index].starts_with("-")) {
        const auto& option = tokens[index++];
        if(option == "--help" || option == "-h") {
            parsed.help = true;
            continue;
        }
        if(option != "--root" && option != "--store" && option != "--format") {
            return usage_error("unknown option: " + option);
        }
        if(index >= tokens.size() || tokens[index].starts_with("-")) {
            return usage_error("missing value for " + option);
        }
        const auto value = tokens[index++];
        if(option == "--root") parsed.root = value;
        else if(option == "--store") parsed.store = value;
        else parsed.format = value;
    }

    if(parsed.root.empty() && index < tokens.size()) {
        parsed.root = tokens[index++];
    }
    if(parsed.format != "json") {
        return usage_error("unsupported format: " + parsed.format);
    }
    if(parsed.help) {
        SkillCliParseResult result;
        result.arguments = std::move(parsed);
        return result;
    }
    if(parsed.root.empty()) return usage_error("a skills root is required");
    if(index >= tokens.size()) return usage_error("a command is required");

    parsed.command = tokens[index++];
    parsed.operands.assign(tokens.begin() + static_cast<std::ptrdiff_t>(index), tokens.end());
    SkillCliParseResult result;
    result.arguments = std::move(parsed);
    return result;
}

std::string skillctl_usage() {
    return "usage: skillctl [--root ROOT] [--store STORE] [--format json] COMMAND [ARGS...]\n"
           "       skillctl ROOT COMMAND [ARGS...]";
}

} // namespace agent_framework
