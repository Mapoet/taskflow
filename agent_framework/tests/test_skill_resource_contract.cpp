#include <agent/skill_manifest.hpp>
#include <agent/skill_resource.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

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
    for(const auto& issue : issues) if(issue.code == code) return true;
    return false;
}

const SkillResourceDescriptor& find_resource(const SkillManifest& manifest,
                                             SkillResourceType kind) {
    for(const auto& resource : manifest.resources) if(resource.kind == kind) return resource;
    assert(false && "resource kind not found");
    return manifest.resources.front();
}

std::string manifest_yaml() {
    return R"YAML(api-version: agent.taskflow/v1
kind: Skill
name: stage7-contract
version: 1.0.0
description: Stage 7 contract fixture
permissions:
  filesystem:
    read: [.]
  secrets: [api-token]
resources:
  references:
    - id: guide
      path: references/guide.md
      media-type: text/markdown
      read-mode: text
      cache-policy: on-demand
      depends-on: [asset]
      permissions:
        filesystem:
          read: [.]
      citation:
        title: GNSS Guide
        authors:
          - Naifeng Fu
        published: "2026"
        url: https://example.org/guide
        locator: section
      index:
        kind: lexical-v1
  assets:
    - id: asset
      path: assets/data.bin
      media-type: application/octet-stream
      read-mode: binary
      sha256: d59386e0ae435e292fbe0ebcdb954b75ed5fb3922091277cb19f798fc5d50718
      size: 5
      license: CC-BY-4.0
      source: package://assets/data.bin
      cache-policy: on-demand
  models:
    - id: model
      path: models/model.bin
      media-type: application/octet-stream
      read-mode: mmap
      sha256: 9372c470eeadd5ecd9c3c74c2b3cb633f8e2f2fad799250a0f70d652b6b825e4
      size: 5
      license: Apache-2.0
      source: package://models/model.bin
      cache-policy: pin
      runtime: onnxruntime
      requirements:
        devices:
          - cpu
          - cuda
        precisions:
          - fp32
          - fp16
        min-memory-bytes: 2147483648
)YAML";
}
} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / "agent_skill_resource_contract_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    write_file(base / "references/guide.md", "GNSS reference");
    write_file(base / "assets/data.bin", "asset");
    write_file(base / "models/model.bin", "model");

    const auto parsed = parse_skill_manifest_yaml(manifest_yaml());
    assert(parsed.manifest);
    assert(!has_issue(parsed.issues, "invalid_resource_read_mode"));

    const auto& reference = find_resource(*parsed.manifest, SkillResourceType::Reference);
    assert(reference.read_mode == SkillResourceReadMode::Text);
    assert(reference.cache_policy == SkillCachePolicy::OnDemand);
    assert(reference.citation.has_value());
    assert(reference.citation->title == "GNSS Guide");
    assert(reference.citation->authors.size() == 1U);
    assert(reference.index.has_value());
    assert(reference.index->kind == "lexical-v1");
    assert(reference.depends_on == std::vector<std::string>({"asset"}));
    assert(reference.permissions.filesystem_read == std::vector<std::string>({"."}));

    const auto& asset = find_resource(*parsed.manifest, SkillResourceType::Asset);
    assert(asset.declared_size == 5U);
    assert(asset.license == "CC-BY-4.0");
    assert(asset.source_uri == "package://assets/data.bin");
    assert(asset.read_mode == SkillResourceReadMode::Binary);

    const auto& model = find_resource(*parsed.manifest, SkillResourceType::Model);
    assert(model.read_mode == SkillResourceReadMode::MemoryMap);
    assert(model.cache_policy == SkillCachePolicy::Pin);
    assert(model.model_requirements.has_value());
    assert(model.model_requirements->devices.size() == 2U);
    assert(model.model_requirements->precisions.size() == 2U);
    assert(model.model_requirements->min_memory_bytes == 2147483648ULL);

    const auto validation = validate_skill_manifest(*parsed.manifest, base);
    for(const auto& issue : validation) assert(!issue.error);
    const auto normalized = skill_manifest_to_json(*parsed.manifest);
    const auto& model_json = normalized.at("resources").at("models").at(0);
    assert(model_json.at("read_mode") == "mmap");
    assert(model_json.at("cache_policy") == "pin");
    assert(model_json.at("size") == 5U);
    assert(model_json.at("requirements").at("min_memory_bytes") == 2147483648ULL);
    const auto& reference_json = normalized.at("resources").at("references").at(0);
    assert(reference_json.at("depends_on").at(0) == "asset");
    assert(reference_json.at("permissions").at("filesystem").at("read").at(0) == ".");

    auto excessive_permission = *parsed.manifest;
    excessive_permission.resources.front().permissions.secrets = {"undeclared-token"};
    assert(has_issue(validate_skill_manifest(excessive_permission, base),
                     "resource_permission_exceeds_manifest"));

    auto invalid_dependencies = *parsed.manifest;
    invalid_dependencies.resources.front().depends_on = {
        invalid_dependencies.resources.front().id, "missing", "missing"};
    const auto dependency_issues = validate_skill_manifest(invalid_dependencies, base);
    assert(has_issue(dependency_issues, "resource_dependency_self_reference"));
    assert(has_issue(dependency_issues, "resource_dependency_missing"));
    assert(has_issue(dependency_issues, "duplicate_resource_dependency"));

    auto missing_audit = *parsed.manifest;
    for(auto& resource : missing_audit.resources) {
        if(resource.kind == SkillResourceType::Asset) {
            resource.sha256.clear();
            resource.declared_size.reset();
            resource.license.clear();
            resource.source_uri.clear();
        }
    }
    assert(has_issue(validate_skill_manifest(missing_audit, base),
                     "resource_digest_required"));
    assert(has_issue(validate_skill_manifest(missing_audit, base),
                     "resource_size_required"));
    assert(has_issue(validate_skill_manifest(missing_audit, base),
                     "resource_license_required"));
    assert(has_issue(validate_skill_manifest(missing_audit, base),
                     "resource_source_required"));

    auto wrong_size = *parsed.manifest;
    for(auto& resource : wrong_size.resources) {
        if(resource.kind == SkillResourceType::Model) resource.declared_size = 6U;
    }
    assert(has_issue(validate_skill_manifest(wrong_size, base),
                     "resource_size_mismatch"));

    auto executable_model = *parsed.manifest;
    for(auto& resource : executable_model.resources) {
        if(resource.kind == SkillResourceType::Model) resource.executable = true;
    }
    assert(has_issue(validate_skill_manifest(executable_model, base),
                     "model_executable_forbidden"));

    auto invalid_metadata = *parsed.manifest;
    for(auto& resource : invalid_metadata.resources) {
        if(resource.kind == SkillResourceType::Asset) {
            resource.source_uri = "relative/path";
            resource.license = "invalid\nlicense";
            resource.cache_policy = SkillCachePolicy::Unknown;
        }
        if(resource.kind == SkillResourceType::Reference) {
            resource.read_mode = SkillResourceReadMode::Text;
            resource.media_type = "application/octet-stream";
            resource.index->kind = "embedding-v1";
        }
        if(resource.kind == SkillResourceType::Model) {
            resource.runtime.clear();
            resource.model_requirements.reset();
        }
    }
    const auto invalid_issues = validate_skill_manifest(invalid_metadata, base);
    assert(has_issue(invalid_issues, "invalid_resource_source"));
    assert(has_issue(invalid_issues, "invalid_resource_license"));
    assert(has_issue(invalid_issues, "invalid_resource_cache_policy"));
    assert(has_issue(invalid_issues, "resource_read_mode_media_type_mismatch"));
    assert(has_issue(invalid_issues, "unsupported_reference_index"));
    assert(has_issue(invalid_issues, "model_runtime_required"));
    assert(has_issue(invalid_issues, "model_requirements_required"));

    const auto invalid_enum = parse_skill_manifest_yaml(
        "api-version: agent.taskflow/v1\nkind: Skill\nname: invalid-mode\nversion: 1.0.0\n"
        "description: invalid\nresources:\n  references:\n    - id: guide\n"
        "      path: references/guide.md\n      read-mode: whole-file\n");
    assert(invalid_enum.manifest);
    assert(has_issue(validate_skill_manifest(*invalid_enum.manifest, base),
                     "invalid_resource_read_mode"));

    const auto overflow_size = parse_skill_manifest_yaml(
        "api-version: agent.taskflow/v1\nkind: Skill\nname: overflow\nversion: 1.0.0\n"
        "description: invalid\nresources:\n  assets:\n    - id: asset\n"
        "      path: assets/data.bin\n"
        "      sha256: d59386e0ae435e292fbe0ebcdb954b75ed5fb3922091277cb19f798fc5d50718\n"
        "      size: 18446744073709551616\n      license: MIT\n"
        "      source: package://assets/data.bin\n");
    assert(overflow_size.manifest);
    assert(has_issue(validate_skill_manifest(*overflow_size.manifest, base),
                     "resource_size_required"));

    const auto legacy = parse_skill_manifest_yaml(
        "name: legacy\ndescription: legacy\nassets:\n  - assets/data.bin\n");
    assert(legacy.manifest && legacy.manifest->legacy_v0);
    const auto legacy_issues = validate_skill_manifest(*legacy.manifest, base);
    assert(!has_issue(legacy_issues, "resource_digest_required"));
    assert(!has_issue(legacy_issues, "resource_size_required"));

    fs::remove_all(base, ec);
    std::cout << "test_skill_resource_contract: ok\n";
    return 0;
}
