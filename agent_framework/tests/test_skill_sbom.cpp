#include "agent/skill_sbom.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace fs = std::filesystem;
using namespace agent_framework;

namespace {
void write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << value;
    assert(output.good());
}

SkillResourceDescriptor resource(const std::string& id, SkillResourceType kind,
                                 const std::string& path) {
    SkillResourceDescriptor value;
    value.id = id;
    value.kind = kind;
    value.path = path;
    return value;
}
}

int main() {
    auto manifest = std::make_shared<SkillManifest>();
    manifest->name = "org.example.audit";
    manifest->version = "1.0.0";
    manifest->resources = {
        resource("script", SkillResourceType::Script, "scripts/run.sh"),
        resource("cli", SkillResourceType::Cli, "bin/tool"),
        resource("mcp", SkillResourceType::Mcp, "mcp/server.json"),
        resource("asset", SkillResourceType::Asset, "assets/data.bin"),
        resource("model", SkillResourceType::Model, "models/model.onnx")};
    manifest->dependencies.push_back({"org.example.dep", "^2.0.0", false});
    SkillPackageRecord record;
    record.id = manifest->name;
    record.version = *SkillSemVersion::parse(manifest->version);
    record.manifest = manifest;
    for(const auto& resource : manifest->resources)
        record.resource_digests[resource.path] = std::string(64, 'a');
    const auto sbom = generate_skill_sbom(record);
    assert(sbom.at("bomFormat") == "CycloneDX" && sbom.at("specVersion") == "1.6");
    assert(sbom.at("components").size() == 5);
    std::set<std::string> types;
    for(const auto& component : sbom.at("components"))
        types.insert(component.at("type").get<std::string>());
    assert(types.count("application") && types.count("file") && types.count("machine-learning-model"));
    assert(sbom.at("dependencies").at(0).at("dependsOn").at(0) ==
           "pkg:taskflow/org.example.dep@^2.0.0");

    SkillProvenanceOptions provenance{"https://github.com/example/audit", "abc123", "ci/test"};
    const auto statement = generate_skill_provenance(record, provenance);
    assert(statement.at("apiVersion") == "agent.taskflow/skill-provenance/v1");
    assert(statement.at("source").at("revision") == "abc123");
    assert(statement.at("materials").size() == 5);

    const auto root = fs::temp_directory_path() /
        ("taskflow-skill-sbom-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto package = root / "package";
    write(package / "data.txt", "payload");
    write(package / "SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: audit-package
version: 1.0.0
description: audited package
resources:
  references:
    - id: data
      path: data.txt
---
body
)");
    const auto archive = root / "audit.tfskill";
    auto built = build_audited_skill_archive(package, archive, provenance);
    assert(built.ok);
    assert(built.metadata.archive_digest == built.archive.archive_digest);
    assert(built.metadata.entry_count == 4);
    assert(built.metadata.sbom_digest.size() == 64);
    const auto extracted = root / "extracted";
    assert(extract_skill_archive(archive, extracted).ok);
    assert(fs::exists(extracted / "META-INF" / "sbom.cdx.json"));
    assert(fs::exists(extracted / "META-INF" / "provenance.json"));
    assert(inspect_audited_skill_archive(archive, built.metadata).ok);
    auto wrong_metadata = built.metadata;
    wrong_metadata.sbom_digest = std::string(64, '0');
    assert(!inspect_audited_skill_archive(archive, wrong_metadata).ok);

    auto raw = build_skill_archive(package, root / "raw.tfskill");
    assert(raw.ok);
    auto raw_metadata = built.metadata;
    raw_metadata.archive_digest = raw.archive_digest;
    raw_metadata.entry_count = raw.entries.size();
    assert(!inspect_audited_skill_archive(root / "raw.tfskill", raw_metadata).ok);

    auto second = build_audited_skill_archive(package, root / "audit-2.tfskill", provenance);
    assert(second.ok && second.archive.archive_digest == built.archive.archive_digest);
    SkillProvenanceOptions invalid;
    assert(!build_audited_skill_archive(package, root / "invalid.tfskill", invalid).ok);

    fs::remove_all(root);
    std::cout << "skill SBOM and provenance tests passed\n";
}
