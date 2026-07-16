#include <agent/skills/skill_doctor.hpp>

#include <agent/skills/skill_capability_runtime.hpp>
#include <agent/skills/skill_lifecycle.hpp>

#include <algorithm>
#include <fstream>

namespace agent_framework {

namespace {

void add_error(SkillDoctorReport& report, std::string code,
               const std::filesystem::path& path, std::string location,
               std::string message, std::string suggestion) {
    SkillDiagnostic diagnostic;
    diagnostic.severity = SkillDiagnosticSeverity::Error;
    diagnostic.code = std::move(code);
    diagnostic.path = path;
    diagnostic.location = std::move(location);
    diagnostic.message = std::move(message);
    diagnostic.suggestion = std::move(suggestion);
    report.diagnostics.push_back(std::move(diagnostic));
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool executable(const std::filesystem::path& path) {
    std::error_code ec;
    const auto permissions = std::filesystem::status(path, ec).permissions();
    if(ec) return false;
    using perms = std::filesystem::perms;
    return (permissions & (perms::owner_exec | perms::group_exec | perms::others_exec)) !=
           perms::none;
}

bool command_available(const std::string& command, const SkillDoctorOptions& options) {
    if(contains(options.available_executables, command)) return true;
    const std::filesystem::path path(command);
    return path.is_absolute() && std::filesystem::is_regular_file(path) && executable(path);
}

nlohmann::json diagnostic_json(const SkillDiagnostic& diagnostic) {
    return {{"severity", diagnostic.severity == SkillDiagnosticSeverity::Error ? "error" : "warning"},
            {"code", diagnostic.code}, {"path", diagnostic.path.generic_string()},
            {"location", diagnostic.location}, {"message", diagnostic.message},
            {"suggestion", diagnostic.suggestion}};
}

} // namespace

nlohmann::json SkillDoctorReport::to_json() const {
    auto serialized = nlohmann::json::array();
    for(const auto& diagnostic : diagnostics) serialized.push_back(diagnostic_json(diagnostic));
    return {{"ready", ready}, {"checks", checks}, {"diagnostics", std::move(serialized)}};
}

SkillDoctorReport SkillDoctor::inspect(const std::string& skill_id,
                                       const SkillDoctorOptions& options) const {
    SkillDoctorReport report;
    const auto entry = registry_->get(skill_id);
    if(!entry || !entry->manifest) {
        add_error(report, "skill_doctor_skill_not_found", {}, "/skill",
                  "skill is not available", "scan or install the skill first");
        return report;
    }
    const auto package = entry->script_jail.value_or(entry->file_path.parent_path());
    const auto& manifest = *entry->manifest;
    const SkillPolicyEngine policy(manifest.permissions, options.grants, package);
    std::size_t audit_complete = 0;
    for(std::size_t index = 0; index < manifest.resources.size(); ++index) {
        const auto& resource = manifest.resources[index];
        const auto path = package / resource.path;
        const auto location = "/resources/" + std::to_string(index);
        const bool runtime_required = resource.kind == SkillResourceType::Script ||
                                      resource.kind == SkillResourceType::Cli ||
                                      resource.kind == SkillResourceType::Model;
        if(resource.kind == SkillResourceType::Asset || resource.kind == SkillResourceType::Model) {
            if(resource.sha256.empty() || !resource.declared_size || resource.license.empty() ||
               resource.source_uri.empty())
                add_error(report, "skill_doctor_audit_metadata_incomplete", path, location,
                          "resource audit metadata is incomplete",
                          "declare sha256, size, license and source");
            else ++audit_complete;
        }
        if(runtime_required &&
           (resource.runtime.empty() || !contains(options.available_runtimes, resource.runtime)))
            add_error(report, "skill_doctor_runtime_missing", path, location + "/runtime",
                      "a declared runtime is unavailable", "provision the declared runtime");
        if(resource.kind == SkillResourceType::Cli &&
           (!std::filesystem::is_regular_file(path) || !executable(path)))
            add_error(report, "skill_doctor_cli_unavailable", path, location,
                      "CLI resource is missing or not executable", "install and mark the CLI executable");
        if(resource.kind == SkillResourceType::Model &&
           (!std::filesystem::is_regular_file(path) || !contains(options.available_models, resource.id)))
            add_error(report, "skill_doctor_model_unavailable", path, location,
                      "model resource is unavailable", "provision the declared model resource");
        if(resource.kind == SkillResourceType::Model && resource.model_requirements) {
            const auto& requirements = *resource.model_requirements;
            const auto intersects = [](const auto& left, const auto& right) {
                return std::any_of(left.begin(), left.end(), [&](const auto& value) {
                    return std::find(right.begin(), right.end(), value) != right.end();
                });
            };
            if(!requirements.devices.empty() &&
               !intersects(requirements.devices, options.available_devices))
                add_error(report, "skill_doctor_model_device_incompatible", path, location,
                          "model device requirement is incompatible", "declare an available device");
            if(!requirements.precisions.empty() &&
               !intersects(requirements.precisions, options.available_precisions))
                add_error(report, "skill_doctor_model_precision_incompatible", path, location,
                          "model precision requirement is incompatible", "declare an available precision");
            if(requirements.min_memory_bytes > options.available_memory_bytes)
                add_error(report, "skill_doctor_model_memory_insufficient", path, location,
                          "model memory requirement is incompatible", "declare sufficient memory");
        }
        if(resource.kind == SkillResourceType::Mcp && std::filesystem::is_regular_file(path)) {
            try {
                std::ifstream input(path);
                const auto value = nlohmann::json::parse(input);
                const auto descriptor = skill_parse_mcp_descriptor(value);
                if(descriptor.transport == "stdio" &&
                   !command_available(descriptor.command, options))
                    add_error(report, "skill_doctor_mcp_command_unavailable", path,
                              location + "/command", "MCP stdio command is unavailable",
                              "provision the declared command or executable");
                if(descriptor.transport == "http" &&
                   !policy.authorize_network(descriptor.url).allowed)
                    add_error(report, "skill_doctor_mcp_network_unauthorized", path,
                              location + "/url", "MCP HTTP origin is not authorized",
                              "request and grant the descriptor origin");
                for(const auto& [target, reference] : descriptor.secret_references) {
                    (void)target;
                    if(!policy.authorize_secret(reference).allowed)
                        add_error(report, "skill_doctor_mcp_secret_unauthorized", path,
                                  location + "/secret-references",
                                  "MCP secret reference is not authorized",
                                  "request and grant the referenced secret");
                }
            } catch(const std::exception&) {
                add_error(report, "skill_doctor_mcp_malformed", path, location,
                          "MCP descriptor is malformed", "provide a valid JSON object descriptor");
            }
        }
        if(!resource.sha256.empty() && std::filesystem::is_regular_file(path)) {
            std::string digest_error;
            const auto digest = skill_sha256_file(path, &digest_error);
            if(!digest || *digest != resource.sha256)
                add_error(report, "skill_doctor_digest_mismatch", path, location + "/sha256",
                          "resource digest does not match", "recompute or restore the declared resource");
        }
    }
    for(const auto& requested : manifest.permissions.tools) {
        if(!policy.authorize_tool(requested).allowed)
            add_error(report, "skill_doctor_tool_grant_insufficient", entry->file_path,
                      "/permissions/tools", "tool grant is insufficient",
                      "grant the declared tool");
    }
    for(const auto& requested : manifest.permissions.network) {
        if(!policy.authorize_network(requested).allowed)
            add_error(report, "skill_doctor_network_grant_insufficient", entry->file_path,
                      "/permissions/network", "network grant is insufficient",
                      "grant the declared network origin");
    }
    for(const auto& requested : manifest.permissions.environment) {
        if(!policy.authorize_environment(requested).allowed)
            add_error(report, "skill_doctor_environment_grant_insufficient", entry->file_path,
                      "/permissions/environment", "environment grant is insufficient",
                      "grant the declared environment variable");
    }
    for(const auto& requested : manifest.permissions.filesystem_read) {
        if(!policy.authorize_filesystem(requested, false).allowed)
            add_error(report, "skill_doctor_filesystem_grant_insufficient", entry->file_path,
                      "/permissions/filesystem/read", "filesystem read grant is insufficient",
                      "grant the declared filesystem scope");
    }
    for(const auto& requested : manifest.permissions.filesystem_write) {
        if(!policy.authorize_filesystem(requested, true).allowed)
            add_error(report, "skill_doctor_filesystem_grant_insufficient", entry->file_path,
                      "/permissions/filesystem/write", "filesystem write grant is insufficient",
                      "grant the declared filesystem scope");
    }
    for(const auto& requested : manifest.permissions.secrets) {
        if(!policy.authorize_secret(requested).allowed)
            add_error(report, "skill_doctor_secret_grant_insufficient", entry->file_path,
                      "/permissions/secrets", "secret grant is insufficient",
                      "grant the declared secret reference");
    }
    bool cache_integrity = false;
    std::size_t cache_objects = 0;
    if(cache_) {
        const auto cache_report = cache_->inspect();
        cache_integrity = cache_report.ok;
        cache_objects = cache_report.object_count;
        if(!cache_report.ok)
            add_error(report, "skill_doctor_cache_corrupt", cache_->root(), "/cache",
                      "resource cache integrity check failed", "run skillctl cache verify");
    }
    std::sort(report.diagnostics.begin(), report.diagnostics.end(), [](const auto& left, const auto& right) {
        return std::tie(left.path, left.location, left.code) < std::tie(right.path, right.location, right.code);
    });
    report.ready = report.diagnostics.empty();
    report.checks = {{"offline", true}, {"resources", manifest.resources.size()},
                     {"errors", report.diagnostics.size()},
                     {"secretsDeclared", manifest.permissions.secrets.size()},
                     {"auditMetadataComplete", audit_complete},
                     {"cachePresent", static_cast<bool>(cache_)},
                     {"cacheIntegrity", cache_integrity}, {"cacheObjects", cache_objects},
                     {"automaticExecution", false}};
    return report;
}

} // namespace agent_framework
