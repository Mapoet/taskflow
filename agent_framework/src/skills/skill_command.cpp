#include <agent/skills/skill_command.hpp>
#include <agent/skills/skill_lifecycle.hpp>
#include <agent/skills/skill_package_gate.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <limits>

namespace agent_framework {

namespace {

nlohmann::json diagnostic_to_json(const SkillDiagnostic& diagnostic) {
    return {
        {"severity", diagnostic.severity == SkillDiagnosticSeverity::Error ? "error" : "warning"},
        {"code", diagnostic.code},
        {"path", diagnostic.path.generic_string()},
        {"location", diagnostic.location},
        {"message", diagnostic.message},
        {"suggestion", diagnostic.suggestion},
    };
}

SkillCliParseResult usage_error(std::string message) {
    SkillCliParseResult result;
    result.response.exit = SkillCliExit::Usage;
    result.response.command = "parse";
    result.response.error = {
        {"code", "skillctl_usage_error"},
        {"message", std::move(message)},
        {"details", {{"usage", skillctl_usage()}}},
    };
    return result;
}

nlohmann::json entry_to_json(const SkillIndexEntry& entry) {
    return {{"id", entry.id}, {"name", entry.name}, {"description", entry.description},
            {"version", entry.version}, {"license", entry.license}, {"tags", entry.tags},
            {"triggerKeywords", entry.trigger_keywords}, {"scripts", entry.scripts},
            {"references", entry.references}, {"cli", entry.cli_programs},
            {"allowedTools", entry.allowed_tools},
            {"disableModelInvocation", entry.disable_model_invocation}};
}

SkillCommandResponse command_error(SkillCliExit exit, std::string command,
                                   std::string code, std::string message,
                                   nlohmann::json details = nlohmann::json::object()) {
    SkillCommandResponse response;
    response.exit = exit;
    response.command = std::move(command);
    response.error = {{"code", std::move(code)}, {"message", std::move(message)},
                      {"details", std::move(details)}};
    return response;
}

nlohmann::json package_record_to_json(const SkillPackageRecord& package) {
    return {{"id", package.id}, {"version", package.version.str()},
            {"packageDigest", package.package_digest},
            {"resourceDigests", package.resource_digests},
            {"sourceUri", package.source_uri},
            {"signatureIdentity", package.signature_identity},
            {"archiveDigest", package.archive_digest}, {"publisher", package.publisher},
            {"keyId", package.key_id}, {"signatureDigest", package.signature_digest},
            {"sbomDigest", package.sbom_digest}, {"provenanceDigest", package.provenance_digest},
            {"registryDigest", package.registry_digest}, {"legacyUnsigned", package.legacy_unsigned}};
}

SkillCliExit lifecycle_exit(const std::string& command, const nlohmann::json& error) {
    const auto code = error.value("code", "");
    const auto message = error.value("message", error.value("error", ""));
    if(message.find("digest is invalid") != std::string::npos)
        return SkillCliExit::IntegrityFailed;
    if(code == kSkillDependencyConflict || code == kSkillPackageInUse)
        return message.find("unavailable") != std::string::npos
            ? SkillCliExit::NotFound : SkillCliExit::OperationFailed;
    if(code == kSkillDigestMismatch || code == kSkillLockInvalid)
        return SkillCliExit::IntegrityFailed;
    if(command == "install" || command == "update")
        return SkillCliExit::IntegrityFailed;
    return SkillCliExit::NotFound;
}

SkillCommandResponse package_gate_error(const std::string& command,
                                        const SkillPackageGateResult& gate) {
    const auto code = gate.error.value("code", kSkillLifecycleInvalid);
    const auto message = gate.error.value(
        "message", gate.error.value("error", "package preflight failed"));
    const auto exit = code == "skill_package_tests_failed" ||
                      code == "skill_package_validation_failed" ||
                      code == "skill_package_lint_failed"
        ? SkillCliExit::ContractFailed : SkillCliExit::IntegrityFailed;
    auto response = command_error(exit, command, code, message,
                                  gate.error.value("details", nlohmann::json::object()));
    auto preflight = gate.to_json();
    preflight.erase("ok");
    preflight.erase("error");
    preflight.erase("diagnostics");
    response.data = {{"preflight", std::move(preflight)}};
    response.diagnostics = gate.diagnostics;
    return response;
}

template <typename Operation>
SkillCommandResponse run_lifecycle(const std::shared_ptr<SkillRegistry>& registry,
                                   const std::filesystem::path& store,
                                   const std::string& command,
                                   Operation&& operation) {
    if(store.empty())
        return command_error(SkillCliExit::Usage, command, "skillctl_usage_error",
                             "lifecycle commands require --store");
    try {
        SkillLifecycleManager manager(registry, store);
        auto result = std::invoke(std::forward<Operation>(operation), manager);
        if(!result.ok) {
            const auto code = result.error.value("code", kSkillLifecycleInvalid);
            const auto message = result.error.value(
                "message", result.error.value("error", "lifecycle operation failed"));
            return command_error(lifecycle_exit(command, result.error), command, code,
                                 message,
                                 result.error.value("details", nlohmann::json::object()));
        }
        const auto lock = result.lockfile.value_or(manager.lockfile()).to_json();
        SkillCommandResponse response;
        response.command = command;
        response.data = {{"roots", lock.at("roots")},
                         {"rootRanges", lock.at("rootRanges")},
                         {"packages", lock.at("packages")},
                         {"edges", lock.at("edges")},
                         {"generation", registry->snapshot().generation()}};
        if(result.package) response.data["package"] = package_record_to_json(*result.package);
        return response;
    } catch(const std::exception& exception) {
        return command_error(SkillCliExit::IntegrityFailed, command, kSkillLockInvalid,
                             exception.what());
    }
}

SkillResourceType resource_type(SkillResourceKind kind) {
    switch(kind) {
        case SkillResourceKind::Script: return SkillResourceType::Script;
        case SkillResourceKind::Cli: return SkillResourceType::Cli;
        case SkillResourceKind::Reference: return SkillResourceType::Reference;
        case SkillResourceKind::Tool: return SkillResourceType::Tool;
        case SkillResourceKind::Mcp: return SkillResourceType::Mcp;
        case SkillResourceKind::Template: return SkillResourceType::Template;
        case SkillResourceKind::Schema: return SkillResourceType::Schema;
        case SkillResourceKind::Prompt: return SkillResourceType::Prompt;
        case SkillResourceKind::Workflow: return SkillResourceType::Workflow;
        case SkillResourceKind::Config: return SkillResourceType::Config;
        case SkillResourceKind::Asset: return SkillResourceType::Asset;
        case SkillResourceKind::Model: return SkillResourceType::Model;
        case SkillResourceKind::Test: return SkillResourceType::Test;
        case SkillResourceKind::AnyDeclared: return SkillResourceType::Unknown;
    }
    return SkillResourceType::Unknown;
}

bool valid_utf8_text(const std::string& value) {
    std::size_t index = 0;
    while(index < value.size()) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if(byte == 0) return false;
        if(byte < 0x80) {
            if(byte < 0x09 || (byte > 0x0D && byte < 0x20)) return false;
            ++index;
            continue;
        }
        std::size_t continuation = 0;
        if((byte & 0xE0) == 0xC0) continuation = 1;
        else if((byte & 0xF0) == 0xE0) continuation = 2;
        else if((byte & 0xF8) == 0xF0) continuation = 3;
        else return false;
        if(index + continuation >= value.size()) return false;
        for(std::size_t offset = 1; offset <= continuation; ++offset) {
            if((static_cast<unsigned char>(value[index + offset]) & 0xC0) != 0x80) return false;
        }
        index += continuation + 1;
    }
    return true;
}

std::string base64_encode(const std::string& input) {
    static constexpr std::array<char, 64> alphabet = {
        'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P',
        'Q','R','S','T','U','V','W','X','Y','Z','a','b','c','d','e','f',
        'g','h','i','j','k','l','m','n','o','p','q','r','s','t','u','v',
        'w','x','y','z','0','1','2','3','4','5','6','7','8','9','+','/'
    };
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for(std::size_t index = 0; index < input.size(); index += 3) {
        const auto a = static_cast<unsigned char>(input[index]);
        const auto b = index + 1 < input.size() ? static_cast<unsigned char>(input[index + 1]) : 0;
        const auto c = index + 2 < input.size() ? static_cast<unsigned char>(input[index + 2]) : 0;
        const auto value = (static_cast<unsigned>(a) << 16) |
                           (static_cast<unsigned>(b) << 8) | static_cast<unsigned>(c);
        output.push_back(alphabet[(value >> 18) & 0x3F]);
        output.push_back(alphabet[(value >> 12) & 0x3F]);
        output.push_back(index + 1 < input.size() ? alphabet[(value >> 6) & 0x3F] : '=');
        output.push_back(index + 2 < input.size() ? alphabet[value & 0x3F] : '=');
    }
    return output;
}

std::string inferred_media_type(const std::string& path, bool text) {
    const auto extension = std::filesystem::path(path).extension().string();
    if(extension == ".md") return "text/markdown";
    if(extension == ".json") return "application/json";
    if(extension == ".yaml" || extension == ".yml") return "application/yaml";
    if(extension == ".txt" || extension == ".sh" || text) return "text/plain";
    return "application/octet-stream";
}

void add_lint_warning(std::vector<SkillDiagnostic>& diagnostics, std::string code,
                      const std::filesystem::path& path, std::string location,
                      std::string message, std::string suggestion) {
    SkillDiagnostic diagnostic;
    diagnostic.severity = SkillDiagnosticSeverity::Warning;
    diagnostic.code = std::move(code);
    diagnostic.path = path;
    diagnostic.location = std::move(location);
    diagnostic.message = std::move(message);
    diagnostic.suggestion = std::move(suggestion);
    diagnostics.push_back(std::move(diagnostic));
}

std::vector<std::string> sorted_unique(std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

std::vector<std::string> intersection(const std::vector<std::string>& requested,
                                      const std::vector<std::string>& granted) {
    const auto left = sorted_unique(requested);
    const auto right = sorted_unique(granted);
    std::vector<std::string> result;
    std::set_intersection(left.begin(), left.end(), right.begin(), right.end(),
                          std::back_inserter(result));
    return result;
}

std::vector<std::string> difference(const std::vector<std::string>& requested,
                                    const std::vector<std::string>& granted) {
    const auto left = sorted_unique(requested);
    const auto right = sorted_unique(granted);
    std::vector<std::string> result;
    std::set_difference(left.begin(), left.end(), right.begin(), right.end(),
                        std::back_inserter(result));
    return result;
}

nlohmann::json permission_set_json(const SkillPermissionSet& permissions,
                                   bool redact_secrets = true) {
    return {{"tools", sorted_unique(permissions.tools)},
            {"network", sorted_unique(permissions.network)},
            {"environment", sorted_unique(permissions.environment)},
            {"filesystemRead", sorted_unique(permissions.filesystem_read)},
            {"filesystemWrite", sorted_unique(permissions.filesystem_write)},
            {"secrets", permissions.secrets.empty() || !redact_secrets
                ? sorted_unique(permissions.secrets)
                : std::vector<std::string>{"<redacted>"}}};
}

SkillPermissionSet as_permission_set(const SkillPermissionGrant& grant) {
    return {grant.tools, grant.network, grant.environment, grant.filesystem_read,
            grant.filesystem_write, grant.secrets};
}

SkillPermissionSet effective_permissions(const SkillPermissionSet& requested,
                                         const SkillPermissionGrant& granted) {
    return {intersection(requested.tools, granted.tools),
            intersection(requested.network, granted.network),
            intersection(requested.environment, granted.environment),
            intersection(requested.filesystem_read, granted.filesystem_read),
            intersection(requested.filesystem_write, granted.filesystem_write),
            intersection(requested.secrets, granted.secrets)};
}

SkillPermissionSet denied_permissions(const SkillPermissionSet& requested,
                                      const SkillPermissionGrant& granted) {
    return {difference(requested.tools, granted.tools),
            difference(requested.network, granted.network),
            difference(requested.environment, granted.environment),
            difference(requested.filesystem_read, granted.filesystem_read),
            difference(requested.filesystem_write, granted.filesystem_write),
            difference(requested.secrets, granted.secrets)};
}

} // namespace

bool SkillCommandResponse::ok() const noexcept {
    return exit == SkillCliExit::Success;
}

nlohmann::json SkillCommandResponse::to_json() const {
    auto serialized_diagnostics = nlohmann::json::array();
    for(const auto& diagnostic : diagnostics) {
        serialized_diagnostics.push_back(diagnostic_to_json(diagnostic));
    }

    return {
        {"apiVersion", "agent.taskflow/skillctl-output/v1"},
        {"command", command},
        {"ok", ok()},
        {"data", data},
        {"diagnostics", std::move(serialized_diagnostics)},
        {"error", error},
    };
}

SkillCliParseResult parse_skill_cli_arguments(const std::vector<std::string>& tokens) {
    if(tokens.size() == 1 && (tokens.front() == "--help" || tokens.front() == "-h")) {
        SkillCliParseResult result;
        result.arguments = SkillCliArguments{};
        result.arguments->help = true;
        return result;
    }

    SkillCliArguments parsed;
    std::size_t index = 0;
    while(index < tokens.size() && tokens[index].starts_with("-")) {
        const auto& option = tokens[index++];
        if(option == "--help" || option == "-h") {
            parsed.help = true;
            continue;
        }
        if(option != "--root" && option != "--store" && option != "--format") {
            return usage_error("unknown option: " + option);
        }
        if(index >= tokens.size() || tokens[index].starts_with("-")) {
            return usage_error("missing value for " + option);
        }
        const auto value = tokens[index++];
        if(option == "--root") parsed.root = value;
        else if(option == "--store") parsed.store = value;
        else parsed.format = value;
    }

    if(parsed.root.empty() && index < tokens.size()) {
        parsed.root = tokens[index++];
    }
    if(parsed.format != "json") {
        return usage_error("unsupported format: " + parsed.format);
    }
    if(parsed.help) {
        SkillCliParseResult result;
        result.arguments = std::move(parsed);
        return result;
    }
    if(parsed.root.empty()) return usage_error("a skills root is required");
    if(index >= tokens.size()) return usage_error("a command is required");

    parsed.command = tokens[index++];
    parsed.operands.assign(tokens.begin() + static_cast<std::ptrdiff_t>(index), tokens.end());
    SkillCliParseResult result;
    result.arguments = std::move(parsed);
    return result;
}

std::string skillctl_usage() {
    return "usage: skillctl [--root ROOT] [--store STORE] [--format json] COMMAND [ARGS...]\n"
           "       skillctl ROOT COMMAND [ARGS...]\n"
           "       skillctl ROOT reference page ID RESOURCE --offset N --max-bytes N\n"
           "       skillctl ROOT reference search ID RESOURCE QUERY --limit N\n"
           "       skillctl ROOT cache status|verify|gc\n"
           "       skillctl ROOT cache pin|unpin DIGEST\n"
           "       skillctl ROOT model check ID RESOURCE --runtime R --device D --precision P --memory N\n"
           "       skillctl ROOT package build SOURCE OUTPUT --source URI --revision REV [--builder ID]\n"
           "       skillctl ROOT package inspect|sbom PATH\n"
           "       skillctl ROOT package sign ARCHIVE SIG --key KEY --publisher P --source URI\n"
           "       skillctl ROOT package verify ARCHIVE SIG --trust TRUST [--now EPOCH]\n"
           "       skillctl ROOT registry sync INDEX SIG TRUST SOURCE\n"
           "       skillctl ROOT registry resolve INDEX SIG TRUST SOURCE ID VERSION";
}

SkillCommandService::SkillCommandService(std::shared_ptr<SkillRegistry> registry,
                                         std::filesystem::path cache_root)
    : registry_(std::move(registry)), loader_(*registry_) {
    if(cache_root.empty()) {
        const char* configured = std::getenv("AGENT_SKILL_CACHE_DIR");
        cache_root = configured && *configured ? std::filesystem::path(configured)
            : std::filesystem::temp_directory_path() / "taskflow-skill-cache";
    }
    resource_access_ = std::make_shared<SkillResourceAccess>();
    resource_cache_ = std::make_shared<SkillResourceCache>(cache_root);
    references_ = std::make_shared<SkillReferenceService>(
        SkillReferenceLimits{}, cache_root / "derived");
    models_ = std::make_shared<SkillModelService>(resource_cache_);
}

SkillCommandResponse SkillCommandService::list() const {
    auto entries = registry_->entries();
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    auto skills = nlohmann::json::array();
    for(const auto& entry : entries) skills.push_back(entry_to_json(entry));
    SkillCommandResponse response;
    response.command = "list";
    response.data = {{"skills", std::move(skills)},
                     {"generation", registry_->snapshot().generation()}};
    return response;
}

SkillCommandResponse SkillCommandService::validate() const {
    SkillCommandResponse response;
    response.command = "validate";
    response.diagnostics = registry_->diagnostics();
    response.data = {{"valid", registry_->valid()}, {"skills", registry_->entries().size()},
                     {"generation", registry_->snapshot().generation()}};
    if(!registry_->valid()) {
        response.exit = SkillCliExit::ContractFailed;
        response.error = {{"code", "skill_contract_failed"},
                          {"message", "the skill registry contains contract errors"},
                          {"details", nlohmann::json::object()}};
    }
    return response;
}

SkillCommandResponse SkillCommandService::show(const std::string& skill_id) const {
    const auto entry = registry_->get(skill_id);
    if(!entry) return command_error(SkillCliExit::NotFound, "show", "skill_not_found",
                                    "skill not found", {{"skill", skill_id}});
    SkillCommandResponse response;
    response.command = "show";
    response.data = {{"skill", entry_to_json(*entry)}};
    return response;
}

SkillCommandResponse SkillCommandService::inspect(const std::string& skill_id,
                                                   bool resolved) const {
    const auto manifest = registry_->get_manifest(skill_id);
    if(!manifest) return command_error(SkillCliExit::NotFound, "inspect", "skill_not_found",
                                       "skill not found", {{"skill", skill_id}});
    SkillCommandResponse response;
    response.command = "inspect";
    response.data = {{"manifest", skill_manifest_to_json(*manifest, resolved)}};
    return response;
}

SkillCommandResponse SkillCommandService::read(const std::string& skill_id,
                                                SkillResourceKind kind,
                                                const std::string& relative_path,
                                                std::size_t max_bytes, bool raw) const {
    if(!registry_->get(skill_id)) {
        return command_error(SkillCliExit::NotFound, "read", "skill_not_found",
                             "skill not found", {{"skill", skill_id}});
    }
    std::string loader_error;
    const auto content = loader_.load_resource(
        skill_id, relative_path, kind, max_bytes, &loader_error);
    if(!content) return command_error(SkillCliExit::OperationFailed, "read",
                                      "skill_resource_read_failed", loader_error,
                                      {{"skill", skill_id}, {"path", relative_path}});

    const bool text = valid_utf8_text(*content);
    std::string media_type;
    if(const auto manifest = registry_->get_manifest(skill_id)) {
        const auto requested = resource_type(kind);
        for(const auto& resource : manifest->resources) {
            if(resource.path == relative_path &&
               (kind == SkillResourceKind::AnyDeclared || resource.kind == requested)) {
                media_type = resource.media_type;
                break;
            }
        }
    }
    if(media_type.empty()) media_type = inferred_media_type(relative_path, text);

    SkillCommandResponse response;
    response.command = "read";
    response.data = {{"mediaType", media_type}, {"bytes", content->size()},
                     {"encoding", text ? "utf-8" : "base64"},
                     {"content", text ? *content : base64_encode(*content)}};
    if(raw) response.raw_output = *content;
    return response;
}

SkillCommandResponse SkillCommandService::lint(const std::string& skill_id,
                                                bool warnings_as_errors) const {
    std::vector<SkillIndexEntry> entries;
    if(skill_id.empty()) entries = registry_->entries();
    else if(const auto entry = registry_->get(skill_id)) entries.push_back(*entry);
    else return command_error(SkillCliExit::NotFound, "lint", "skill_not_found",
                              "skill not found", {{"skill", skill_id}});

    SkillCommandResponse response;
    response.command = "lint";
    for(const auto& entry : entries) {
        const auto& path = entry.file_path;
        const auto& manifest = entry.manifest;
        if(!manifest) continue;
        if(manifest->license.empty())
            add_lint_warning(response.diagnostics, "skill_lint_missing_license", path,
                             "/license", "license metadata is missing", "declare an SPDX license");
        if(manifest->authors.empty())
            add_lint_warning(response.diagnostics, "skill_lint_missing_authors", path,
                             "/authors", "author metadata is missing", "declare at least one author");
        if(!manifest->legacy_v0 && !SkillSemVersion::parse(manifest->version))
            add_lint_warning(response.diagnostics, "skill_lint_invalid_version", path,
                             "/version", "version is not valid SemVer", "use MAJOR.MINOR.PATCH");

        std::vector<std::string> tool_resources;
        for(std::size_t index = 0; index < manifest->resources.size(); ++index) {
            const auto& resource = manifest->resources[index];
            const std::string location = "/resources/" + std::to_string(index);
            if(resource.kind == SkillResourceType::Tool) tool_resources.push_back(resource.id);
            if(resource.media_type.empty())
                add_lint_warning(response.diagnostics, "skill_lint_missing_media_type", path,
                                 location + "/media-type", "resource media type is missing",
                                 "declare media-type explicitly");
            if((resource.kind == SkillResourceType::Script ||
                resource.kind == SkillResourceType::Cli ||
                resource.kind == SkillResourceType::Model) && resource.runtime.empty())
                add_lint_warning(response.diagnostics, "skill_lint_missing_runtime", path,
                                 location + "/runtime", "executable resource runtime is missing",
                                 "declare the required runtime");
            if((resource.kind == SkillResourceType::Script ||
                resource.kind == SkillResourceType::Tool) &&
               (resource.input_schema.empty() || resource.output_schema.empty()))
                add_lint_warning(response.diagnostics, "skill_lint_missing_schema", path,
                                 location, "callable resource schema is incomplete",
                                 "declare input-schema and output-schema");
            if(resource.sha256.empty())
                add_lint_warning(response.diagnostics, "skill_lint_missing_digest", path,
                                 location + "/sha256", "resource digest is missing",
                                 "declare the lowercase SHA-256 digest");
        }
        for(std::size_t index = 0; index < manifest->dependencies.size(); ++index) {
            const auto& dependency = manifest->dependencies[index];
            if(dependency.version.empty() || dependency.version == "*" ||
               !SkillSemVersionRange::parse(dependency.version))
                add_lint_warning(response.diagnostics, "skill_lint_unbounded_dependency", path,
                                 "/dependencies/" + std::to_string(index) + "/version",
                                 "dependency range is unbounded or invalid",
                                 "use a bounded SemVer range");
        }
        for(const auto& permission : manifest->permissions.tools) {
            if(std::find(tool_resources.begin(), tool_resources.end(), permission) == tool_resources.end())
                add_lint_warning(response.diagnostics, "skill_lint_unused_permission", path,
                                 "/permissions/tools", "tool permission is not tied to a declared tool",
                                 "remove the permission or declare the tool resource");
        }
    }
    std::sort(response.diagnostics.begin(), response.diagnostics.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.path, left.location, left.code) <
                         std::tie(right.path, right.location, right.code);
              });
    response.data = {{"skills", entries.size()}, {"warnings", response.diagnostics.size()},
                     {"warningsAsErrors", warnings_as_errors}};
    if(warnings_as_errors && !response.diagnostics.empty()) {
        response.exit = SkillCliExit::ContractFailed;
        response.error = {{"code", "skill_lint_failed"},
                          {"message", "lint warnings were promoted to errors"},
                          {"details", {{"warnings", response.diagnostics.size()}}}};
    }
    return response;
}

SkillCommandResponse SkillCommandService::graph() const {
    auto entries = registry_->entries();
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    auto packages = nlohmann::json::array();
    auto edges = nlohmann::json::array();
    nlohmann::json digests = nlohmann::json::object();
    std::vector<std::string> depended;
    for(const auto& entry : entries) {
        packages.push_back({{"id", entry.id}, {"version", entry.version},
                            {"digest", entry.package_digest}});
        digests[entry.id] = entry.package_digest;
        if(!entry.manifest) continue;
        for(const auto& dependency : entry.manifest->dependencies) {
            edges.push_back({{"from", entry.id}, {"to", dependency.name},
                             {"range", dependency.version}, {"optional", dependency.optional}});
            depended.push_back(dependency.name);
        }
    }
    std::sort(edges.begin(), edges.end(), [](const auto& left, const auto& right) {
        return std::tie(left.at("from"), left.at("to"), left.at("range")) <
               std::tie(right.at("from"), right.at("to"), right.at("range"));
    });
    depended = sorted_unique(std::move(depended));
    auto roots = nlohmann::json::array();
    nlohmann::json root_ranges = nlohmann::json::object();
    for(const auto& entry : entries) {
        if(!std::binary_search(depended.begin(), depended.end(), entry.id)) {
            roots.push_back(entry.id);
            root_ranges[entry.id] = entry.version.empty() ? "*" : entry.version;
        }
    }
    SkillCommandResponse response;
    response.command = "graph";
    response.data = {{"roots", std::move(roots)}, {"rootRanges", std::move(root_ranges)},
                     {"packages", std::move(packages)}, {"edges", std::move(edges)},
                     {"digests", std::move(digests)},
                     {"generation", registry_->snapshot().generation()}};
    return response;
}

SkillCommandResponse SkillCommandService::permissions(
    const std::string& skill_id, const SkillPermissionGrant& granted) const {
    const auto manifest = registry_->get_manifest(skill_id);
    if(!manifest) return command_error(SkillCliExit::NotFound, "permissions",
                                       "skill_not_found", "skill not found",
                                       {{"skill", skill_id}});
    const auto effective = effective_permissions(manifest->permissions, granted);
    const auto denied = denied_permissions(manifest->permissions, granted);
    SkillCommandResponse response;
    response.command = "permissions";
    response.data = {{"skill", skill_id},
                     {"declared", permission_set_json(manifest->permissions)},
                     {"granted", permission_set_json(as_permission_set(granted))},
                     {"effective", permission_set_json(effective)},
                     {"denied", permission_set_json(denied)}};
    return response;
}

SkillCommandResponse SkillCommandService::doctor(const std::string& skill_id,
                                                  const SkillDoctorOptions& options) const {
    if(!registry_->get(skill_id))
        return command_error(SkillCliExit::NotFound, "doctor", "skill_not_found",
                             "skill not found", {{"skill", skill_id}});
    SkillDoctor doctor_service(registry_, resource_cache_);
    const auto report = doctor_service.inspect(skill_id, options);
    SkillCommandResponse response;
    response.command = "doctor";
    response.diagnostics = report.diagnostics;
    response.data = {{"skill", skill_id}, {"ready", report.ready}, {"checks", report.checks}};
    if(!report.ready) {
        const auto has = [&](const std::string& code) {
            return std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
                               [&](const auto& diagnostic) { return diagnostic.code == code; });
        };
        if(has("skill_doctor_digest_mismatch")) response.exit = SkillCliExit::IntegrityFailed;
        else if(has("skill_doctor_runtime_missing") || has("skill_doctor_model_unavailable") ||
                has("skill_doctor_model_device_incompatible") ||
                has("skill_doctor_model_precision_incompatible") ||
                has("skill_doctor_model_memory_insufficient"))
            response.exit = SkillCliExit::DependencyUnavailable;
        else response.exit = SkillCliExit::OperationFailed;
        response.error = {{"code", "skill_doctor_failed"},
                          {"message", "offline readiness checks failed"},
                          {"details", {{"errors", report.diagnostics.size()}}}};
    }
    return response;
}

SkillCommandResponse SkillCommandService::test(const std::string& skill_id,
                                                const std::string& filter,
                                                std::size_t jobs) const {
    if(jobs == 0 || jobs > 64)
        return command_error(SkillCliExit::Usage, "test", "skillctl_usage_error",
                             "jobs must be in the range 1..64", {{"jobs", jobs}});

    std::vector<std::string> skill_ids;
    if(!skill_id.empty()) {
        if(!registry_->get(skill_id))
            return command_error(SkillCliExit::NotFound, "test", "skill_not_found",
                                 "skill not found", {{"skill", skill_id}});
        skill_ids.push_back(skill_id);
    } else {
        for(const auto& entry : registry_->entries()) {
            if(entry.manifest && std::any_of(
                   entry.manifest->resources.begin(), entry.manifest->resources.end(),
                   [](const auto& resource) { return resource.kind == SkillResourceType::Test; }))
                skill_ids.push_back(entry.id);
        }
        std::sort(skill_ids.begin(), skill_ids.end());
    }

    SkillTestRunner runner(registry_);
    auto cases = nlohmann::json::array();
    std::size_t passed = 0;
    std::size_t failed = 0;
    bool matched = false;
    for(const auto& id : skill_ids) {
        SkillTestRunOptions options;
        options.filter = filter;
        options.jobs = jobs;
        const auto suite = runner.run(id, options);
        const auto code = suite.error.is_object() ? suite.error.value("code", "") : "";
        if(code == "skill_test_filter_unmatched") continue;
        if(code == "skill_test_options_invalid")
            return command_error(SkillCliExit::Usage, "test", "skillctl_usage_error",
                                 suite.error.value("message", "invalid test options"));
        if(!suite.error.is_null() && code != "skill_tests_failed")
            return command_error(SkillCliExit::ContractFailed, "test",
                                 code.empty() ? "skill_test_execution_failed" : code,
                                 suite.error.value("message", "test execution failed"),
                                 suite.error.value("details", nlohmann::json::object()));
        matched = matched || !suite.cases.empty();
        passed += suite.passed;
        failed += suite.failed;
        for(const auto& test_case : suite.cases) {
            auto serialized = test_case.to_json();
            serialized["skill"] = id;
            cases.push_back(std::move(serialized));
        }
    }
    if(!matched)
        return command_error(SkillCliExit::Usage, "test", "skill_test_filter_unmatched",
                             "no tests matched the requested filter", {{"filter", filter}});

    std::sort(cases.begin(), cases.end(), [](const auto& left, const auto& right) {
        return std::tie(left.at("skill"), left.at("name")) <
               std::tie(right.at("skill"), right.at("name"));
    });
    SkillCommandResponse response;
    response.command = "test";
    response.data = {{"passed", passed}, {"failed", failed}, {"cases", std::move(cases)}};
    if(failed != 0) {
        response.exit = SkillCliExit::ContractFailed;
        response.error = {{"code", "skill_tests_failed"},
                          {"message", "one or more skill tests failed"},
                          {"details", {{"failed", failed}}}};
    }
    return response;
}

SkillCommandResponse SkillCommandService::install(
    const std::filesystem::path& store, const std::filesystem::path& package,
    const std::string& source_uri, const std::string& signature_identity) const {
    if(store.empty()) return command_error(SkillCliExit::Usage, "install", "skillctl_usage_error",
                                           "lifecycle commands require --store");
    const auto gate = SkillPackageGate{}.inspect(package);
    if(!gate.ok) return package_gate_error("install", gate);
    auto response = run_lifecycle(registry_, store, "install", [&](SkillLifecycleManager& manager) {
        SkillInstallOptions options;
        options.source_uri = source_uri;
        options.signature_identity = signature_identity;
        return manager.install(package, options);
    });
    if(response.ok()) {
        auto preflight = gate.to_json();
        preflight.erase("ok");
        preflight.erase("error");
        preflight.erase("diagnostics");
        response.data["preflight"] = std::move(preflight);
        response.diagnostics = gate.diagnostics;
    }
    return response;
}

SkillCommandResponse SkillCommandService::update(
    const std::filesystem::path& store, const std::filesystem::path& package,
    const std::string& source_uri, const std::string& signature_identity) const {
    if(store.empty()) return command_error(SkillCliExit::Usage, "update", "skillctl_usage_error",
                                           "lifecycle commands require --store");
    const auto gate = SkillPackageGate{}.inspect(package);
    if(!gate.ok) return package_gate_error("update", gate);
    auto response = run_lifecycle(registry_, store, "update", [&](SkillLifecycleManager& manager) {
        SkillInstallOptions options;
        options.source_uri = source_uri;
        options.signature_identity = signature_identity;
        return manager.update(package, options);
    });
    if(response.ok()) {
        auto preflight = gate.to_json();
        preflight.erase("ok");
        preflight.erase("error");
        preflight.erase("diagnostics");
        response.data["preflight"] = std::move(preflight);
        response.diagnostics = gate.diagnostics;
    }
    return response;
}

SkillCommandResponse SkillCommandService::package(const std::filesystem::path& package_path) const {
    const auto gate = SkillPackageGate{}.inspect(package_path);
    if(!gate.ok) return package_gate_error("package", gate);
    SkillCommandResponse response;
    response.command = "package";
    response.data = gate.to_json();
    response.data.erase("ok");
    response.data.erase("error");
    response.data.erase("diagnostics");
    response.diagnostics = gate.diagnostics;
    return response;
}

SkillCommandResponse SkillCommandService::enable(
    const std::filesystem::path& store, const std::string& skill_id,
    const std::string& range) const {
    return run_lifecycle(registry_, store, "enable", [&](SkillLifecycleManager& manager) {
        return manager.enable(skill_id, range);
    });
}

SkillCommandResponse SkillCommandService::disable(
    const std::filesystem::path& store, const std::string& skill_id) const {
    return run_lifecycle(registry_, store, "disable", [&](SkillLifecycleManager& manager) {
        return manager.disable(skill_id);
    });
}

SkillCommandResponse SkillCommandService::remove(
    const std::filesystem::path& store, const std::string& package_digest) const {
    return run_lifecycle(registry_, store, "remove", [&](SkillLifecycleManager& manager) {
        return manager.remove(package_digest);
    });
}

SkillCommandResponse SkillCommandService::rollback(
    const std::filesystem::path& store, const std::string& skill_id) const {
    return run_lifecycle(registry_, store, "rollback", [&](SkillLifecycleManager& manager) {
        return manager.rollback(skill_id);
    });
}

SkillCommandResponse SkillCommandService::reference_page(
    const std::string& skill_id, const std::string& resource_id,
    std::uint64_t offset, std::size_t max_bytes) const {
    const auto entry = registry_->get(skill_id);
    if(!entry || !entry->manifest)
        return command_error(SkillCliExit::NotFound, "reference page", "skill_not_found",
                             "skill not found", {{"skill", skill_id}});
    SkillResourceOpenOptions options;
    options.mode = SkillResourceReadMode::Stream;
    options.max_bytes = max_bytes;
    auto opened = resource_access_->open_snapshot(*entry, entry->manifest, resource_id, options);
    if(!opened.ok)
        return command_error(SkillCliExit::OperationFailed, "reference page",
                             opened.error.value("code", "skill_resource_open_failed"),
                             opened.error.value("message", "resource open failed"));
    auto page = references_->read_page(*opened.handle, offset, max_bytes);
    if(!page.ok)
        return command_error(SkillCliExit::OperationFailed, "reference page",
                             page.error.value("code", "skill_reference_failed"),
                             page.error.value("message", "reference paging failed"));
    const auto& value = *page.page;
    SkillCommandResponse response;
    response.command = "reference page";
    response.data = {{"text", value.text}, {"nextOffset", value.next_offset},
                     {"eof", value.eof},
                     {"citation", {{"resourceId", value.citation.resource_id},
                         {"resourceDigest", value.citation.resource_digest},
                         {"packageDigest", value.citation.package_digest},
                         {"source", value.citation.source_uri},
                         {"license", value.citation.license},
                         {"byteStart", value.citation.byte_start},
                         {"byteEnd", value.citation.byte_end}}}};
    return response;
}

SkillCommandResponse SkillCommandService::reference_search(
    const std::string& skill_id, const std::string& resource_id,
    const std::string& query, std::size_t limit) const {
    const auto entry = registry_->get(skill_id);
    if(!entry || !entry->manifest)
        return command_error(SkillCliExit::NotFound, "reference search", "skill_not_found",
                             "skill not found", {{"skill", skill_id}});
    auto opened = resource_access_->open_snapshot(*entry, entry->manifest, resource_id);
    if(!opened.ok)
        return command_error(SkillCliExit::OperationFailed, "reference search",
                             opened.error.value("code", "skill_resource_open_failed"),
                             opened.error.value("message", "resource open failed"));
    auto searched = references_->search(*opened.handle, query, limit);
    if(!searched.ok)
        return command_error(SkillCliExit::OperationFailed, "reference search",
                             searched.error.value("code", "skill_reference_search_failed"),
                             searched.error.value("message", "reference search failed"));
    auto hits = nlohmann::json::array();
    for(const auto& hit : searched.hits)
        hits.push_back({{"score", hit.score}, {"matchStart", hit.match_start},
                        {"matchEnd", hit.match_end}, {"snippet", hit.snippet},
                        {"snippetStart", hit.snippet_start}, {"snippetEnd", hit.snippet_end},
                        {"citation", {{"resourceDigest", hit.citation.resource_digest},
                                      {"byteStart", hit.citation.byte_start},
                                      {"byteEnd", hit.citation.byte_end}}}});
    SkillCommandResponse response;
    response.command = "reference search";
    response.data = {{"hits", std::move(hits)},
                     {"indexPath", searched.index_path.generic_string()}};
    return response;
}

SkillCommandResponse SkillCommandService::cache_status() const {
    const auto report = resource_cache_->inspect();
    if(!report.ok)
        return command_error(SkillCliExit::IntegrityFailed, "cache status",
                             report.error.value("code", "skill_cache_failed"),
                             report.error.value("message", "cache inspection failed"));
    SkillCommandResponse response;
    response.command = "cache status";
    response.data = {{"objects", report.object_count}, {"bytes", report.total_bytes},
                     {"pinnedBytes", report.pinned_bytes},
                     {"leasedObjects", report.leased_objects}};
    return response;
}

SkillCommandResponse SkillCommandService::cache_verify() const {
    const auto result = resource_cache_->verify();
    if(!result.ok)
        return command_error(SkillCliExit::IntegrityFailed, "cache verify",
                             result.error.value("code", "skill_cache_verify_failed"),
                             result.error.value("message", "cache verification failed"));
    auto response = cache_status();
    response.command = "cache verify";
    return response;
}

SkillCommandResponse SkillCommandService::cache_gc() const {
    const auto result = resource_cache_->collect();
    if(!result.ok)
        return command_error(SkillCliExit::OperationFailed, "cache gc",
                             result.error.value("code", "skill_cache_gc_failed"),
                             result.error.value("message", "cache collection failed"));
    auto response = cache_status();
    response.command = "cache gc";
    return response;
}

SkillCommandResponse SkillCommandService::cache_pin(const std::string& digest,
                                                     bool pinned) const {
    const auto result = pinned ? resource_cache_->pin(digest) : resource_cache_->unpin(digest);
    const std::string command = pinned ? "cache pin" : "cache unpin";
    if(!result.ok)
        return command_error(SkillCliExit::OperationFailed, command,
                             result.error.value("code", "skill_cache_pin_failed"),
                             result.error.value("message", "cache pin operation failed"));
    SkillCommandResponse response;
    response.command = command;
    response.data = {{"digest", digest}, {"pinned", pinned}};
    return response;
}

SkillCommandResponse SkillCommandService::model_check(
    const std::string& skill_id, const std::string& resource_id,
    const SkillModelHostCapabilities& host) const {
    const auto entry = registry_->get(skill_id);
    if(!entry || !entry->manifest)
        return command_error(SkillCliExit::NotFound, "model check", "skill_not_found",
                             "skill not found", {{"skill", skill_id}});
    SkillResourceOpenOptions options;
    options.mode = SkillResourceReadMode::MemoryMap;
    options.max_bytes = static_cast<std::size_t>(
        std::min<std::uint64_t>(host.max_readonly_bytes,
                               std::numeric_limits<std::size_t>::max()));
    auto opened = resource_access_->open_snapshot(*entry, entry->manifest, resource_id, options);
    if(!opened.ok)
        return command_error(SkillCliExit::OperationFailed, "model check",
                             opened.error.value("code", "skill_resource_open_failed"),
                             opened.error.value("message", "resource open failed"));
    const auto admission = models_->check(*opened.handle, host);
    SkillCommandResponse response;
    response.command = "model check";
    response.data = {{"compatible", admission.compatible},
                     {"codes", admission.codes},
                     {"diagnostics", admission.diagnostics},
                     {"automaticExecution", false}};
    if(!admission.compatible) {
        response.exit = SkillCliExit::DependencyUnavailable;
        response.error = {{"code", "skill_model_incompatible"},
                          {"message", "model is not compatible with the declared host"},
                          {"details", admission.diagnostics}};
    }
    return response;
}

std::optional<SkillResourceKind> parse_skill_resource_kind(const std::string& value) {
    static const std::array<std::pair<const char*, SkillResourceKind>, 13> kinds = {{
        {"script", SkillResourceKind::Script}, {"cli", SkillResourceKind::Cli},
        {"reference", SkillResourceKind::Reference}, {"tool", SkillResourceKind::Tool},
        {"mcp", SkillResourceKind::Mcp}, {"template", SkillResourceKind::Template},
        {"schema", SkillResourceKind::Schema}, {"prompt", SkillResourceKind::Prompt},
        {"workflow", SkillResourceKind::Workflow}, {"config", SkillResourceKind::Config},
        {"asset", SkillResourceKind::Asset}, {"model", SkillResourceKind::Model},
        {"test", SkillResourceKind::Test}
    }};
    const auto found = std::find_if(kinds.begin(), kinds.end(), [&](const auto& item) {
        return value == item.first;
    });
    if(found == kinds.end()) return std::nullopt;
    return found->second;
}

} // namespace agent_framework
