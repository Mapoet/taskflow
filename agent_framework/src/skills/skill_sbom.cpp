#include "agent/skill_sbom.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>

namespace agent_framework {
namespace {

using nlohmann::json;

std::string component_type(SkillResourceType kind) {
    switch(kind) {
    case SkillResourceType::Script:
    case SkillResourceType::Cli:
    case SkillResourceType::Tool:
    case SkillResourceType::Mcp:
    case SkillResourceType::Workflow: return "application";
    case SkillResourceType::Model: return "machine-learning-model";
    default: return "file";
    }
}

std::string package_ref(const SkillPackageRecord& package) {
    return "pkg:taskflow/" + package.id + "@" + package.version.str();
}

std::string canonical(const json& value) { return value.dump() + "\n"; }

void property(json& properties, const std::string& name, const std::string& value) {
    if(!value.empty()) properties.push_back({{"name", name}, {"value", value}});
}

std::string string_list(const std::vector<std::string>& values) {
    json serialized = values;
    return serialized.dump();
}

} // namespace

json generate_skill_sbom(const SkillPackageRecord& package) {
    json components = json::array();
    if(package.manifest) {
        auto resources = package.manifest->resources;
        std::sort(resources.begin(), resources.end(), [](const auto& a, const auto& b) {
            return a.path < b.path;
        });
        for(const auto& resource : resources) {
            json properties = json::array({
                {{"name", "agent.taskflow/resource-kind"}, {"value", to_string(resource.kind)}},
                {{"name", "agent.taskflow/path"}, {"value", resource.path}},
                {{"name", "agent.taskflow/cache-policy"}, {"value", to_string(resource.cache_policy)}},
                {{"name", "agent.taskflow/executable"}, {"value", resource.executable ? "true" : "false"}},
                {{"name", "agent.taskflow/optional"}, {"value", resource.optional ? "true" : "false"}}
            });
            property(properties, "agent.taskflow/runtime", resource.runtime);
            property(properties, "agent.taskflow/media-type", resource.media_type);
            property(properties, "agent.taskflow/source-uri", resource.source_uri);
            property(properties, "agent.taskflow/read-mode", to_string(resource.read_mode));
            property(properties, "agent.taskflow/input-schema", resource.input_schema);
            property(properties, "agent.taskflow/output-schema", resource.output_schema);
            if(resource.declared_size)
                property(properties, "agent.taskflow/declared-size", std::to_string(*resource.declared_size));
            if(resource.size_limit)
                property(properties, "agent.taskflow/size-limit", std::to_string(*resource.size_limit));
            if(!resource.depends_on.empty())
                property(properties, "agent.taskflow/resource-dependencies", string_list(resource.depends_on));
            if(resource.model_requirements) {
                property(properties, "agent.taskflow/model-devices",
                         string_list(resource.model_requirements->devices));
                property(properties, "agent.taskflow/model-precisions",
                         string_list(resource.model_requirements->precisions));
                property(properties, "agent.taskflow/model-min-memory-bytes",
                         std::to_string(resource.model_requirements->min_memory_bytes));
            }
            json component = {{"type", component_type(resource.kind)},
                              {"bom-ref", package_ref(package) + "/" + resource.path},
                              {"name", resource.id}, {"version", package.version.str()},
                              {"properties", std::move(properties)}};
            const auto digest = package.resource_digests.find(resource.path);
            if(digest != package.resource_digests.end())
                component["hashes"] = json::array({{{"alg", "SHA-256"}, {"content", digest->second}}});
            if(!resource.license.empty())
                component["licenses"] = json::array({{{"license", {{"id", resource.license}}}}});
            components.push_back(std::move(component));
        }
        auto declared = package.manifest->dependencies;
        std::sort(declared.begin(), declared.end(), [](const auto& a, const auto& b) {
            return a.name < b.name || (a.name == b.name && a.version < b.version);
        });
        for(const auto& dependency : declared)
            components.push_back({{"type", "library"},
                {"bom-ref", "pkg:taskflow/" + dependency.name + "@" + dependency.version},
                {"name", dependency.name}, {"version", dependency.version},
                {"scope", dependency.optional ? "optional" : "required"}});
    }
    json dependencies = json::array();
    json depends_on = json::array();
    if(package.manifest) {
        auto declared = package.manifest->dependencies;
        std::sort(declared.begin(), declared.end(), [](const auto& a, const auto& b) {
            return a.name < b.name || (a.name == b.name && a.version < b.version);
        });
        for(const auto& dependency : declared)
            depends_on.push_back("pkg:taskflow/" + dependency.name + "@" + dependency.version);
    }
    dependencies.push_back({{"ref", package_ref(package)}, {"dependsOn", depends_on}});
    if(package.manifest) {
        for(const auto& resource : package.manifest->resources) {
            json resource_dependencies = json::array();
            for(const auto& dependency_id : resource.depends_on) {
                const auto dependency = std::find_if(package.manifest->resources.begin(),
                    package.manifest->resources.end(), [&](const auto& candidate) {
                        return candidate.id == dependency_id;
                    });
                if(dependency != package.manifest->resources.end())
                    resource_dependencies.push_back(package_ref(package) + "/" + dependency->path);
            }
            dependencies.push_back({{"ref", package_ref(package) + "/" + resource.path},
                                    {"dependsOn", std::move(resource_dependencies)}});
        }
    }
    return {{"bomFormat", "CycloneDX"}, {"specVersion", "1.6"}, {"version", 1},
            {"metadata", {{"component", {{"type", "application"},
                {"bom-ref", package_ref(package)}, {"name", package.id},
                {"version", package.version.str()}}}}},
            {"components", components}, {"dependencies", dependencies}};
}

json generate_skill_provenance(const SkillPackageRecord& package,
                               const SkillProvenanceOptions& options) {
    json materials = json::array();
    for(const auto& [path, digest] : package.resource_digests)
        materials.push_back({{"path", path}, {"sha256", digest}});
    return {{"apiVersion", "agent.taskflow/skill-provenance/v1"},
            {"builder", {{"id", options.builder_id}}},
            {"source", {{"uri", options.source_uri}, {"revision", options.source_revision}}},
            {"subject", {{"id", package.id}, {"version", package.version.str()}}},
            {"materials", materials}};
}

SkillAuditedArchiveResult build_audited_skill_archive(
    const std::filesystem::path& source_directory,
    const std::filesystem::path& output_archive,
    const SkillProvenanceOptions& provenance_options,
    const SkillArchiveLimits& limits) {
    SkillAuditedArchiveResult result;
    if(provenance_options.source_uri.empty() || provenance_options.source_revision.empty() ||
       provenance_options.builder_id.empty()) {
        result.error = std::string(kSkillSupplyChainInvalid) + ": provenance fields are required";
        return result;
    }
    auto inspected = inspect_skill_package(source_directory);
    if(!inspected.ok || !inspected.package || !inspected.package->manifest) {
        result.error = inspected.error.dump();
        return result;
    }
    result.sbom = generate_skill_sbom(*inspected.package);
    result.provenance = generate_skill_provenance(*inspected.package, provenance_options);
    const auto sbom_text = canonical(result.sbom);
    const auto provenance_text = canonical(result.provenance);
    std::string error;
    auto sbom_digest = skill_sha256_bytes(sbom_text, &error);
    auto provenance_digest = skill_sha256_bytes(provenance_text, &error);
    if(!sbom_digest || !provenance_digest) { result.error = error; return result; }
    result.archive = build_skill_archive(source_directory, output_archive,
        {{"META-INF/sbom.cdx.json", sbom_text},
         {"META-INF/provenance.json", provenance_text}}, limits);
    if(!result.archive.ok) { result.error = result.archive.error; return result; }
    result.metadata.package_id = inspected.package->id;
    result.metadata.package_version = inspected.package->version.str();
    result.metadata.archive_digest = result.archive.archive_digest;
    result.metadata.sbom_digest = *sbom_digest;
    result.metadata.provenance_digest = *provenance_digest;
    result.metadata.entry_count = result.archive.entries.size();
    result.ok = true;
    return result;
}

SkillAuditedArchiveResult inspect_audited_skill_archive(
    const std::filesystem::path& archive_path,
    const SkillPackageMetadata& expected,
    const SkillArchiveLimits& limits) {
    SkillAuditedArchiveResult result;
    result.metadata = expected;
    result.archive = inspect_skill_archive(archive_path, limits);
    if(!result.archive.ok) { result.error = result.archive.error; return result; }
    if(result.archive.archive_digest != expected.archive_digest ||
       result.archive.entries.size() != expected.entry_count) {
        result.error = std::string(kSkillSupplyChainInvalid) + ": package metadata mismatch";
        return result;
    }
    bool has_sbom = false, has_provenance = false;
    for(const auto& entry : result.archive.entries) {
        has_sbom = has_sbom || entry.path == "META-INF/sbom.cdx.json";
        has_provenance = has_provenance || entry.path == "META-INF/provenance.json";
    }
    if(!has_sbom || !has_provenance) {
        result.error = std::string(kSkillSupplyChainInvalid) + ": required audit entries are missing";
        return result;
    }
    const auto temporary = std::filesystem::temp_directory_path() /
        ("taskflow-audit-inspect-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    auto extracted = extract_skill_archive(archive_path, temporary, limits);
    if(!extracted.ok) { result.error = extracted.error; return result; }
    std::string error;
    auto sbom_digest = skill_sha256_file(temporary / "META-INF/sbom.cdx.json", &error);
    auto provenance_digest = skill_sha256_file(temporary / "META-INF/provenance.json", &error);
    try {
        std::ifstream sbom_input(temporary / "META-INF/sbom.cdx.json");
        std::ifstream provenance_input(temporary / "META-INF/provenance.json");
        result.sbom = json::parse(sbom_input);
        result.provenance = json::parse(provenance_input);
    } catch(const std::exception& ex) {
        error = ex.what();
    }
    std::error_code ec;
    std::filesystem::remove_all(temporary, ec);
    if(!sbom_digest || !provenance_digest || !error.empty() ||
       *sbom_digest != expected.sbom_digest || *provenance_digest != expected.provenance_digest ||
       result.sbom.value("bomFormat", "") != "CycloneDX" ||
       result.sbom.value("specVersion", "") != "1.6" ||
       result.provenance.value("apiVersion", "") != "agent.taskflow/skill-provenance/v1") {
        result.error = std::string(kSkillSupplyChainInvalid) + ": audit metadata is invalid or mismatched";
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace agent_framework
