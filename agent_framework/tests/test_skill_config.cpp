#include <agent/skills/skill_config.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
namespace fs = std::filesystem;
using namespace agent_framework;

void write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << value;
    assert(output.good());
}

bool code_is(const SkillConfigResult& result, const char* code) {
    return !result.ok && result.error.value("code", "") == code;
}
}

int main() {
    const auto root = fs::temp_directory_path() / "taskflow-skill-config";
    std::error_code ec;
    fs::remove_all(root, ec);
    const auto package = root / "typed-config";
    write(package / "config/defaults.json",
          R"({"endpoint":"https://example.org","retries":2,"credential":"unset"})");
    write(package / "schemas/config.json", R"({
      "type":"object","additionalProperties":false,
      "properties":{"endpoint":{"type":"string"},"retries":{"type":"integer","maximum":5},"credential":{"type":"string","minLength":1}},
      "required":["endpoint","retries","credential"]
    })");
    write(package / "SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: typed-config
version: 1.0.0
description: typed config fixture
permissions:
  secrets: [api-token]
resources:
  schemas:
    - id: config-schema
      path: schemas/config.json
      media-type: application/json
  configs:
    - id: defaults
      path: config/defaults.json
      media-type: application/json
      input-schema: config-schema
      permissions:
        secrets: [api-token]
---
body
)");
    auto registry = std::make_shared<SkillRegistry>(root);
    registry->scan_or_reload();
    assert(registry->valid());
    const auto entry = registry->get("typed-config");
    const auto manifest = registry->get_manifest("typed-config");
    assert(entry && manifest);

    SkillInvocationContext context;
    context.grants.secrets = {"api-token"};
    context.secret_provider = [](std::string_view reference) -> std::optional<std::string> {
        return reference == "api-token" ? std::optional<std::string>("secret-value") : std::nullopt;
    };
    SkillConfigResolveOptions options;
    options.overrides = {{"retries", 4}};
    options.secret_bindings["/credential"] = "api-token";
    SkillConfigService service;
    const auto resolved = service.resolve(*entry, manifest, "defaults", options, context);
    assert(resolved.ok);
    assert(resolved.value.at("endpoint") == "https://example.org");
    assert(resolved.value.at("retries") == 4);
    assert(resolved.value.at("credential") == "secret-value");

    auto invalid = options;
    invalid.overrides = {{"retries", 9}};
    const auto schema_failure = service.resolve(*entry, manifest, "defaults", invalid, context);
    assert(code_is(schema_failure, kSkillInputInvalid));
    assert(schema_failure.error.dump().find("secret-value") == std::string::npos);

    auto denied_context = context;
    denied_context.grants.secrets.clear();
    const auto denied = service.resolve(*entry, manifest, "defaults", options, denied_context);
    assert(code_is(denied, kSkillPermissionDenied));
    assert(denied.error.dump().find("secret-value") == std::string::npos);

    auto missing_target = options;
    missing_target.secret_bindings = {{"/missing", "api-token"}};
    assert(code_is(service.resolve(*entry, manifest, "defaults", missing_target, context),
                   "skill_config_secret_target_missing"));

    fs::remove_all(root, ec);
    std::cout << "skill typed config tests passed\n";
}
