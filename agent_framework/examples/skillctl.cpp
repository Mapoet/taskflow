#include <agent/skill_loader.hpp>
#include <agent/skill_registry.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

using namespace agent_framework;
using json = nlohmann::json;

namespace {
void usage() {
    std::cerr << "usage: skillctl <skills-root> list|validate|show <id>|read <id> <reference|script|cli> <path> [max-bytes]\n";
}

json entry_json(const SkillIndexEntry& e) {
    return {{"id", e.id}, {"name", e.name}, {"description", e.description},
            {"version", e.version}, {"license", e.license}, {"tags", e.tags},
            {"trigger_keywords", e.trigger_keywords}, {"scripts", e.scripts},
            {"references", e.references}, {"cli", e.cli_programs},
            {"allowed_tools", e.allowed_tools},
            {"disable_model_invocation", e.disable_model_invocation}};
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 64; }
    SkillRegistry registry(argv[1]);
    registry.scan_or_reload();
    const std::string command = argv[2];
    if (command == "list") {
        json output = json::array();
        for (const auto& entry : registry.entries()) output.push_back(entry_json(entry));
        std::cout << output.dump(2) << '\n';
        return 0;
    }
    if (command == "validate") {
        json diagnostics = json::array();
        for (const auto& diagnostic : registry.diagnostics()) {
            diagnostics.push_back({{"severity", diagnostic.severity == SkillDiagnosticSeverity::Error ? "error" : "warning"},
                                   {"code", diagnostic.code}, {"path", diagnostic.path.string()},
                                   {"message", diagnostic.message}});
        }
        std::cout << json{{"valid", registry.valid()}, {"skills", registry.entries().size()},
                          {"diagnostics", diagnostics}}.dump(2) << '\n';
        return registry.valid() ? 0 : 2;
    }
    if (command == "show" && argc == 4) {
        const auto entry = registry.get(argv[3]);
        if (!entry) return 3;
        std::cout << entry_json(*entry).dump(2) << '\n';
        return 0;
    }
    if (command == "read" && argc >= 6) {
        SkillResourceKind kind = SkillResourceKind::Reference;
        const std::string kind_text = argv[4];
        if (kind_text == "script") kind = SkillResourceKind::Script;
        else if (kind_text == "cli") kind = SkillResourceKind::Cli;
        else if (kind_text != "reference") { usage(); return 64; }
        const std::size_t max_bytes = argc >= 7 ? std::strtoull(argv[6], nullptr, 10) : 65536;
        SkillLoader loader(registry);
        std::string error;
        auto content = loader.load_resource(argv[3], argv[5], kind, max_bytes, &error);
        if (!content) { std::cerr << error << '\n'; return 4; }
        std::cout << *content;
        return 0;
    }
    usage();
    return 64;
}
