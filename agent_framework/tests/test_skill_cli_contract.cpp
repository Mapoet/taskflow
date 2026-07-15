#include <agent/skill_command.hpp>

#include <cassert>
#include <iostream>

using namespace agent_framework;

int main() {
    SkillCommandResponse response;
    response.exit = SkillCliExit::NotFound;
    response.command = "show";
    response.error = {{"code", "skill_not_found"},
                      {"message", "missing"},
                      {"details", {{"skill", "absent"}}}};

    const auto value = response.to_json();
    assert(value.at("apiVersion") == "agent.taskflow/skillctl-output/v1");
    assert(value.at("command") == "show");
    assert(!value.at("ok").get<bool>());
    assert(value.at("data") == nlohmann::json::object());
    assert(value.at("diagnostics") == nlohmann::json::array());
    assert(value.at("error").at("code") == "skill_not_found");
    assert(static_cast<int>(response.exit) == 3);

    SkillDiagnostic diagnostic;
    diagnostic.severity = SkillDiagnosticSeverity::Error;
    diagnostic.code = "invalid_manifest";
    diagnostic.path = "/tmp/package/SKILL.md";
    diagnostic.location = "/version";
    diagnostic.message = "invalid version";
    diagnostic.suggestion = "use semantic versioning";
    response.diagnostics.push_back(diagnostic);
    const auto with_diagnostic = response.to_json();
    assert(with_diagnostic.at("diagnostics").size() == 1);
    assert(with_diagnostic.at("diagnostics")[0].at("severity") == "error");
    assert(with_diagnostic.at("diagnostics")[0].at("code") == "invalid_manifest");

    SkillCommandResponse success;
    success.command = "list";
    success.data = {{"skills", nlohmann::json::array()}};
    const auto success_value = success.to_json();
    assert(success.ok());
    assert(success_value.at("ok").get<bool>());
    assert(success_value.at("error").is_null());

    const auto legacy = parse_skill_cli_arguments({"/tmp/skills", "list"});
    assert(legacy);
    assert(legacy.arguments->root == "/tmp/skills");
    assert(legacy.arguments->command == "list");

    const auto modern = parse_skill_cli_arguments(
        {"--root", "/tmp/skills", "--store", "/tmp/store", "show", "demo"});
    assert(modern);
    assert(modern.arguments->root == "/tmp/skills");
    assert(modern.arguments->store == "/tmp/store");
    assert(modern.arguments->command == "show");
    assert(modern.arguments->operands == std::vector<std::string>({"demo"}));

    const auto malformed = parse_skill_cli_arguments({"--root", "list"});
    assert(!malformed);
    assert(malformed.response.exit == SkillCliExit::Usage);
    assert(malformed.response.error.at("code") == "skillctl_usage_error");

    const auto unknown = parse_skill_cli_arguments({"--unknown", "value", "list"});
    assert(!unknown);
    assert(unknown.response.exit == SkillCliExit::Usage);

    std::cout << "test_skill_cli_contract: ok\n";
    return 0;
}
