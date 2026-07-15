#include <agent/skill_loader.hpp>
#include <agent/skill_manifest.hpp>
#include <agent/skill_registry.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace {
namespace fs = std::filesystem;
using namespace agent_framework;

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    assert(output.good());
    output << content;
}

bool has_issue(const std::vector<SkillManifestIssue>& issues, const std::string& code) {
    for (const auto& issue : issues) if (issue.code == code) return true;
    return false;
}

const char* manifest_yaml() {
    return R"YAML(api-version: agent.taskflow/v1
kind: Skill
name: complete-skill
version: 1.2.3
description: Complete typed resource fixture
license: Apache-2.0
authors:
  - Naifeng Fu
compatibility:
  agent-framework: ">=2.0 <3.0"
dependencies:
  - name: dependency-skill
    version: "^1.0"
permissions:
  tools:
    - read_file
  network:
    - https://data.example.org
  env:
    - DATA_HOME
  filesystem:
    read:
      - data/
    write:
      - output/
resources:
  scripts:
    - id: run
      path: scripts/run.sh
      input-schema: input
      output-schema: output
      executable: true
  cli:
    - id: helper
      path: cli/helper.sh
  references:
    - id: guide
      path: references/guide.md
  tools:
    - id: local-tool
      path: tools/tool.json
  mcp:
    - id: data-mcp
      path: mcp/server.json
  templates:
    - id: report-template
      path: templates/report.md
  schemas:
    - id: input
      path: schemas/input.json
    - id: output
      path: schemas/output.json
  prompts:
    - id: reviewer
      path: prompts/reviewer.md
  workflows:
    - id: pipeline
      path: workflows/pipeline.yaml
  configs:
    - id: defaults
      path: configs/defaults.json
  assets:
    - id: tiny-asset
      path: assets/tiny.dat
      media-type: application/octet-stream
      read-mode: binary
      sha256: 2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881
      size: 1
      size-limit: 16
      license: Apache-2.0
      source: package://assets/tiny.dat
  models:
    - id: tiny-model
      path: models/model.bin
      media-type: application/octet-stream
      read-mode: mmap
      sha256: 44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a
      size: 2
      license: Apache-2.0
      source: package://models/model.bin
      runtime: test-runtime
      requirements:
        devices:
          - cpu
        precisions:
          - fp32
        min-memory-bytes: 0
tests:
  - id: smoke
    path: tests/smoke.json
x-future-field: preserved
)YAML";
}

void create_resources(const fs::path& root) {
    for (const auto& path : {
             "scripts/run.sh", "cli/helper.sh", "references/guide.md", "tools/tool.json",
             "mcp/server.json", "templates/report.md", "schemas/input.json", "schemas/output.json",
             "prompts/reviewer.md", "workflows/pipeline.yaml", "configs/defaults.json",
             "models/model.bin", "tests/smoke.json"}) write_file(root / path, "{}");
    write_file(root / "assets/tiny.dat", "x");
}
} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / "agent_skill_manifest_v1_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path package = base / "complete";
    create_resources(package);

    auto parsed = parse_skill_manifest_yaml(manifest_yaml());
    assert(parsed.manifest);
    assert(parsed.manifest->resources.size() == 14U);
    assert(has_issue(parsed.issues, "unknown_manifest_field"));
    assert(parsed.manifest->extensions.contains("x-future-field"));
    assert(parsed.manifest->permissions.filesystem_read.size() == 1U);
    assert(parsed.manifest->permissions.filesystem_write.size() == 1U);
    const auto validation = validate_skill_manifest(*parsed.manifest, package);
    for (const auto& issue : validation) assert(!issue.error);
    std::set<SkillResourceType> kinds;
    for (const auto& resource : parsed.manifest->resources) kinds.insert(resource.kind);
    assert(kinds.size() == 13U);
    assert(skill_manifest_to_json(*parsed.manifest).at("resources").contains("workflows"));

    write_file(package / "SKILL.md", std::string("---\n") + manifest_yaml() + "---\n# body\n");
    SkillRegistry registry(base);
    registry.scan_or_reload();
    assert(registry.valid());
    assert(registry.get_manifest("complete-skill"));
    SkillLoader loader(registry);
    std::string error;
    assert(loader.load_resource("complete-skill", "prompts/reviewer.md", SkillResourceKind::Prompt, 64, &error));
    assert(!loader.load_resource("complete-skill", "prompts/reviewer.md", SkillResourceKind::Reference, 64, &error));

    auto duplicate = *parsed.manifest;
    duplicate.resources.push_back(duplicate.resources.front());
    assert(has_issue(validate_skill_manifest(duplicate, package), "duplicate_resource_id"));
    auto bad_hash = *parsed.manifest;
    for (auto& resource : bad_hash.resources)
        if (resource.kind == SkillResourceType::Asset) resource.sha256.assign(64, '0');
    assert(has_issue(validate_skill_manifest(bad_hash, package), "resource_hash_mismatch"));
    auto bad_ref = *parsed.manifest;
    bad_ref.resources.front().input_schema = "missing-schema";
    assert(has_issue(validate_skill_manifest(bad_ref, package), "resource_reference_missing"));

    auto optional = *parsed.manifest;
    SkillResourceDescriptor missing;
    missing.id = "optional-asset";
    missing.kind = SkillResourceType::Asset;
    missing.path = "assets/missing.dat";
    missing.optional = true;
    optional.resources.push_back(missing);
    assert(!has_issue(validate_skill_manifest(optional, package), "resource_missing"));

#if !defined(_WIN32)
    write_file(base / "outside.dat", "outside");
    fs::create_symlink(base / "outside.dat", package / "assets/link.dat", ec);
    if (!ec) {
        auto escaped = *parsed.manifest;
        SkillResourceDescriptor link;
        link.id = "escaped-link";
        link.kind = SkillResourceType::Asset;
        link.path = "assets/link.dat";
        escaped.resources.push_back(link);
        assert(has_issue(validate_skill_manifest(escaped, package), "resource_outside_jail"));
    }
#endif

    const auto unknown = parse_skill_manifest_yaml(
        "api-version: agent.taskflow/v1\nkind: Skill\nname: bad\nversion: 1.0.0\n"
        "description: bad\nresources:\n  unknowns:\n    - path: bad.dat\n");
    assert(has_issue(unknown.issues, "unknown_resource_type"));

    const fs::path invalid = base / "invalid";
    write_file(invalid / "SKILL.md",
               "---\napi-version: agent.taskflow/v1\nkind: Skill\nname: invalid\n"
               "version: broken\ndescription: invalid\nresources:\n  assets:\n"
               "    - id: escape\n      path: ../outside.dat\n---\n");
    registry.scan_or_reload();
    assert(!registry.valid());
    assert(!registry.get("invalid"));

    fs::remove_all(base, ec);
    std::cout << "test_skill_manifest_v1: ok\n";
    return 0;
}
