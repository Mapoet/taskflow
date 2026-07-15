#include <agent/skill_command.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace agent_framework;

namespace {

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    assert(output.good());
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
}

} // namespace

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

    const auto root = std::filesystem::temp_directory_path() / "agent_skill_cli_contract";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    write_file(root / "zeta" / "SKILL.md",
               "---\nname: zeta\ndescription: zeta skill\n---\nzeta\n");
    write_file(root / "alpha" / "references" / "guide.md", "guide-text");
    write_file(root / "alpha" / "assets" / "blob.bin", std::string("a\0b", 3));
    write_file(root / "alpha" / "SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: alpha
version: 1.2.3
description: alpha skill
resources:
  references:
    - id: guide
      path: references/guide.md
      media-type: text/markdown
  assets:
    - id: blob
      path: assets/blob.bin
      media-type: application/octet-stream
---
alpha
)");

    auto registry = std::make_shared<SkillRegistry>(root);
    registry->scan_or_reload();
    SkillCommandService service(registry);
    const auto listed = service.list();
    assert(listed.ok());
    assert(listed.data.at("skills").at(0).at("id") == "alpha");
    assert(listed.data.at("skills").at(1).at("id") == "zeta");
    assert(service.show("absent").exit == SkillCliExit::NotFound);
    const auto inspected = service.inspect("alpha", true);
    assert(inspected.ok());
    assert(inspected.data.at("manifest").at("api_version") == "agent.taskflow/v1");

    const auto bounded = service.read(
        "alpha", SkillResourceKind::Reference, "references/guide.md", 4, false);
    assert(bounded.exit == SkillCliExit::OperationFailed);
    const auto text_read = service.read(
        "alpha", SkillResourceKind::Reference, "references/guide.md", 64, false);
    assert(text_read.data.at("encoding") == "utf-8");
    assert(text_read.data.at("content") == "guide-text");
    const auto binary_read = service.read(
        "alpha", SkillResourceKind::Asset, "assets/blob.bin", 64, false);
    assert(binary_read.data.at("encoding") == "base64");
    assert(binary_read.data.at("content") == "YQBi");
    const auto raw_read = service.read(
        "alpha", SkillResourceKind::Asset, "assets/blob.bin", 64, true);
    assert(raw_read.raw_output == std::optional<std::string>(std::string("a\0b", 3)));

    write_file(root / "duplicate-a" / "SKILL.md", "---\nid: duplicate\n---\na\n");
    write_file(root / "duplicate-b" / "SKILL.md", "---\nid: duplicate\n---\nb\n");
    auto invalid_registry = std::make_shared<SkillRegistry>(root);
    invalid_registry->scan_or_reload();
    SkillCommandService invalid_service(invalid_registry);
    assert(invalid_service.validate().exit == SkillCliExit::ContractFailed);
    std::filesystem::remove_all(root, ec);

    std::cout << "test_skill_cli_contract: ok\n";
    return 0;
}
