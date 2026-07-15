#include <agent/skill_package_gate.hpp>
#include <agent/skill_command.hpp>

#include <algorithm>

namespace agent_framework {
namespace {

SkillIndexEntry package_entry(const SkillPackageRecord& package) {
    SkillIndexEntry entry;
    entry.id = package.id;
    entry.name = package.manifest->name;
    entry.yaml_id = package.manifest->legacy_id;
    entry.description = package.manifest->description;
    entry.version = package.manifest->version;
    entry.license = package.manifest->license;
    entry.tags = package.manifest->tags;
    entry.trigger_keywords = package.manifest->trigger_keywords;
    entry.allowed_tools = package.manifest->permissions.tools;
    entry.disable_model_invocation = package.manifest->disable_model_invocation;
    entry.file_path = package.package_path / "SKILL.md";
    entry.script_jail = package.package_path;
    entry.manifest = package.manifest;
    entry.package_digest = package.package_digest;
    entry.resource_digests = package.resource_digests;
    for(const auto& resource : package.manifest->resources) {
        if(resource.kind == SkillResourceType::Script) entry.scripts.push_back(resource.path);
        if(resource.kind == SkillResourceType::Reference) entry.references.push_back(resource.path);
        if(resource.kind == SkillResourceType::Cli) entry.cli_programs.push_back(resource.path);
    }
    return entry;
}

nlohmann::json diagnostic_json(const SkillDiagnostic& diagnostic) {
    return {{"severity", diagnostic.severity == SkillDiagnosticSeverity::Error ? "error" : "warning"},
            {"code", diagnostic.code}, {"path", diagnostic.path.generic_string()},
            {"location", diagnostic.location}, {"message", diagnostic.message},
            {"suggestion", diagnostic.suggestion}};
}

nlohmann::json package_json(const SkillPackageRecord& package) {
    return {{"id", package.id}, {"version", package.version.str()},
            {"manifest", skill_manifest_to_json(*package.manifest, true)},
            {"packageDigest", package.package_digest},
            {"resourceDigests", package.resource_digests}};
}

SkillPackageGateResult failed(nlohmann::json error) {
    SkillPackageGateResult result;
    if(error.is_object() && error.contains("error") && !error.contains("message")) {
        error["message"] = error.at("error");
        error.erase("error");
    }
    result.error = std::move(error);
    return result;
}

} // namespace

nlohmann::json SkillPackageGateResult::to_json() const {
    auto serialized_diagnostics = nlohmann::json::array();
    for(const auto& diagnostic : diagnostics)
        serialized_diagnostics.push_back(diagnostic_json(diagnostic));
    return {{"ok", ok},
            {"package", package ? package_json(*package) : nlohmann::json(nullptr)},
            {"diagnostics", std::move(serialized_diagnostics)},
            {"tests", tests ? tests->to_json() : nlohmann::json(nullptr)},
            {"error", error}};
}

SkillPackageGateResult SkillPackageGate::inspect(const std::filesystem::path& package) const {
    const auto first = inspect_skill_package(package);
    if(!first.ok || !first.package) return failed(first.error);

    auto registry = std::make_shared<SkillRegistry>(std::vector<std::filesystem::path>{});
    registry->publish({package_entry(*first.package)});
    SkillCommandService service(registry);
    const auto validation = service.validate();
    if(!validation.ok())
        return failed({{"code", "skill_package_validation_failed"},
                       {"message", "package validation failed"},
                       {"details", validation.to_json()}});
    const auto lint = service.lint(first.package->id, false);
    if(!lint.ok())
        return failed({{"code", "skill_package_lint_failed"},
                       {"message", "package lint failed"},
                       {"details", lint.to_json()}});

    SkillPackageGateResult result;
    result.diagnostics = lint.diagnostics;
    if(std::any_of(first.package->manifest->resources.begin(),
                   first.package->manifest->resources.end(), [](const auto& resource) {
                       return resource.kind == SkillResourceType::Test;
                   })) {
        result.tests = SkillTestRunner(registry).run(first.package->id);
        if(!result.tests->ok) {
            result.error = {{"code", "skill_package_tests_failed"},
                            {"message", "package tests failed"},
                            {"details", result.tests->to_json()}};
            return result;
        }
    }

    const auto second = inspect_skill_package(package);
    if(!second.ok || !second.package) return failed(second.error);
    if(first.package->id != second.package->id ||
       first.package->version != second.package->version ||
       first.package->package_digest != second.package->package_digest ||
       first.package->resource_digests != second.package->resource_digests) {
        return failed({{"code", kSkillDigestMismatch},
                       {"message", "package identity changed during preflight"},
                       {"details", {{"first", package_json(*first.package)},
                                    {"second", package_json(*second.package)}}}});
    }
    result.ok = true;
    result.error = nullptr;
    result.package = second.package;
    return result;
}

} // namespace agent_framework
