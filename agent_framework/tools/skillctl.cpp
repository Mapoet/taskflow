#include <agent/skill_command.hpp>
#include <agent/skill_loader.hpp>
#include <agent/skill_registry.hpp>

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace agent_framework;
using json = nlohmann::json;

namespace {

json entry_json(const SkillIndexEntry& entry) {
    return {{"id", entry.id}, {"name", entry.name}, {"description", entry.description},
            {"version", entry.version}, {"license", entry.license}, {"tags", entry.tags},
            {"trigger_keywords", entry.trigger_keywords}, {"scripts", entry.scripts},
            {"references", entry.references}, {"cli", entry.cli_programs},
            {"allowed_tools", entry.allowed_tools},
            {"disable_model_invocation", entry.disable_model_invocation}};
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> tokens;
    for(int i = 1; i < argc; ++i) tokens.emplace_back(argv[i]);
    const auto parsed = parse_skill_cli_arguments(tokens);
    if(!parsed) {
        std::cout << parsed.response.to_json().dump(2) << '\n';
        return static_cast<int>(parsed.response.exit);
    }
    if(parsed.arguments->help) {
        std::cout << skillctl_usage() << '\n';
        return 0;
    }

    const auto& invocation = *parsed.arguments;
    SkillRegistry registry(invocation.root);
    registry.scan_or_reload();
    const auto& command = invocation.command;
    const auto& operands = invocation.operands;
    if(command == "list" && operands.empty()) {
        json output = json::array();
        for(const auto& entry : registry.entries()) output.push_back(entry_json(entry));
        std::cout << output.dump(2) << '\n';
        return 0;
    }
    if(command == "validate" && operands.empty()) {
        json diagnostics = json::array();
        for(const auto& diagnostic : registry.diagnostics()) {
            diagnostics.push_back({{"severity", diagnostic.severity == SkillDiagnosticSeverity::Error ? "error" : "warning"},
                                   {"code", diagnostic.code}, {"path", diagnostic.path.string()},
                                   {"location", diagnostic.location}, {"message", diagnostic.message},
                                   {"suggestion", diagnostic.suggestion}});
        }
        std::cout << json{{"valid", registry.valid()}, {"skills", registry.entries().size()},
                          {"diagnostics", diagnostics}}.dump(2) << '\n';
        return registry.valid() ? 0 : 2;
    }
    if(command == "show" && operands.size() == 1) {
        const auto entry = registry.get(operands[0]);
        if(!entry) return 3;
        std::cout << entry_json(*entry).dump(2) << '\n';
        return 0;
    }
    if(command == "inspect" && operands.size() == 2 && operands[1] == "--resolved") {
        const auto manifest = registry.get_manifest(operands[0]);
        if(!manifest) return 3;
        std::cout << skill_manifest_to_json(*manifest, true).dump(2) << '\n';
        return 0;
    }
    if(command == "read" && operands.size() >= 3) {
        const std::map<std::string, SkillResourceKind> kinds = {
            {"script", SkillResourceKind::Script}, {"cli", SkillResourceKind::Cli},
            {"reference", SkillResourceKind::Reference}, {"tool", SkillResourceKind::Tool},
            {"mcp", SkillResourceKind::Mcp}, {"template", SkillResourceKind::Template},
            {"schema", SkillResourceKind::Schema}, {"prompt", SkillResourceKind::Prompt},
            {"workflow", SkillResourceKind::Workflow}, {"config", SkillResourceKind::Config},
            {"asset", SkillResourceKind::Asset}, {"model", SkillResourceKind::Model},
            {"test", SkillResourceKind::Test}};
        const auto found = kinds.find(operands[1]);
        if(found == kinds.end()) {
            std::cerr << skillctl_usage() << '\n';
            return 64;
        }
        const std::size_t max_bytes = operands.size() >= 4
            ? std::strtoull(operands[3].c_str(), nullptr, 10) : 65536;
        SkillLoader loader(registry);
        std::string error;
        auto content = loader.load_resource(operands[0], operands[2], found->second, max_bytes, &error);
        if(!content) {
            std::cerr << error << '\n';
            return 4;
        }
        std::cout << *content;
        return 0;
    }

    SkillCommandResponse response;
    response.exit = SkillCliExit::Usage;
    response.command = command;
    response.error = {{"code", "skillctl_usage_error"}, {"message", "invalid command arguments"},
                      {"details", {{"usage", skillctl_usage()}}}};
    std::cout << response.to_json().dump(2) << '\n';
    return static_cast<int>(response.exit);
}
