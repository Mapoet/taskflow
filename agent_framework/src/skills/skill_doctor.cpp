#include <agent/skill_doctor.hpp>

#include <agent/skill_lifecycle.hpp>

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
    for(std::size_t index = 0; index < manifest.resources.size(); ++index) {
        const auto& resource = manifest.resources[index];
        const auto path = package / resource.path;
        const auto location = "/resources/" + std::to_string(index);
        const bool runtime_required = resource.kind == SkillResourceType::Script ||
                                      resource.kind == SkillResourceType::Cli ||
                                      resource.kind == SkillResourceType::Model;
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
        if(resource.kind == SkillResourceType::Mcp && std::filesystem::is_regular_file(path)) {
            try {
                std::ifstream input(path);
                const auto value = nlohmann::json::parse(input);
                if(!value.is_object()) throw std::runtime_error("MCP descriptor is not an object");
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
    for(const auto& requested : manifest.permissions.filesystem_read) {
        if(!contains(options.grants.filesystem_read, requested))
            add_error(report, "skill_doctor_filesystem_grant_insufficient", entry->file_path,
                      "/permissions/filesystem/read", "filesystem read grant is insufficient",
                      "grant the declared filesystem scope");
    }
    for(const auto& requested : manifest.permissions.filesystem_write) {
        if(!contains(options.grants.filesystem_write, requested))
            add_error(report, "skill_doctor_filesystem_grant_insufficient", entry->file_path,
                      "/permissions/filesystem/write", "filesystem write grant is insufficient",
                      "grant the declared filesystem scope");
    }
    std::sort(report.diagnostics.begin(), report.diagnostics.end(), [](const auto& left, const auto& right) {
        return std::tie(left.path, left.location, left.code) < std::tie(right.path, right.location, right.code);
    });
    report.ready = report.diagnostics.empty();
    report.checks = {{"offline", true}, {"resources", manifest.resources.size()},
                     {"errors", report.diagnostics.size()},
                     {"secretsDeclared", manifest.permissions.secrets.size()}};
    return report;
}

} // namespace agent_framework
