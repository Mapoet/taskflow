#include <agent/skills/skill_manifest.hpp>

#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
#include <openssl/evp.h>
#endif

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace agent_framework {
namespace {

bool safe_id(const std::string& id) {
    if (id.empty() || id.size() > 128 || id == "." || id == "..") return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.';
    });
}

bool safe_relative(const std::string& value) {
    if (value.empty()) return false;
    const std::filesystem::path path(value);
    if (path.is_absolute()) return false;
    for (const auto& part : path) if (part == "..") return false;
    return true;
}

bool looks_semver(const std::string& value) {
    static const std::regex pattern(
        R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$)");
    return std::regex_match(value, pattern);
}

bool valid_sha256(const std::string& value) {
    return value.size() == 64U && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

bool valid_source_uri(const std::string& value) {
    static const std::regex pattern(R"(^[A-Za-z][A-Za-z0-9+.-]*://[^[:space:]]+$)");
    return value.size() <= 2048U && std::regex_match(value, pattern);
}

bool textual_media_type(const std::string& value) {
    return value.rfind("text/", 0) == 0 || value == "application/json" ||
           value == "application/yaml" || value == "application/xml" ||
           value == "application/markdown";
}

std::string file_sha256(const std::filesystem::path& path) {
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context) return {};
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
    char buffer[16384];
    while (ok && input) {
        input.read(buffer, sizeof(buffer));
        const auto count = input.gcount();
        if (count > 0) ok = EVP_DigestUpdate(context, buffer, static_cast<std::size_t>(count)) == 1;
    }
    ok = ok && EVP_DigestFinal_ex(context, digest, &digest_size) == 1;
    EVP_MD_CTX_free(context);
    if (!ok) return {};
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digest_size; ++i) out << std::setw(2) << static_cast<int>(digest[i]);
    return out.str();
#else
    (void)path;
    return {};
#endif
}

void issue(std::vector<SkillManifestIssue>& out, bool error, std::string code,
           std::string location, std::string message, std::string suggestion = {}) {
    out.push_back({error, std::move(code), std::move(location), std::move(message), std::move(suggestion)});
}

} // namespace

std::vector<SkillManifestIssue> validate_skill_manifest(
    const SkillManifest& manifest, const std::filesystem::path& package_directory) {
    std::vector<SkillManifestIssue> out;
    if (!manifest.legacy_v0 && manifest.api_version != "agent.taskflow/v1")
        issue(out, true, "unsupported_api_version", "/api-version", "unsupported Skill API version", "use agent.taskflow/v1");
    if (manifest.kind != "Skill")
        issue(out, true, "invalid_kind", "/kind", "kind must be Skill", "set kind: Skill");
    const std::string canonical = !manifest.name.empty() ? manifest.name : manifest.legacy_id;
    if ((!manifest.legacy_v0 || !canonical.empty()) && !safe_id(canonical))
        issue(out, true, "invalid_skill_id", "/name", "skill name must match [A-Za-z0-9_.-]{1,128}");
    if (!manifest.legacy_v0 && manifest.version.empty())
        issue(out, true, "missing_version", "/version", "Manifest v1 requires version", "add a SemVer version");
    else if (!manifest.version.empty() && !looks_semver(manifest.version))
        issue(out, true, "invalid_version", "/version", "version is not valid SemVer");
    if (manifest.description.empty())
        issue(out, false, "missing_description", "/description", "skill description is empty");

    std::unordered_set<std::string> resource_ids;
    std::unordered_map<std::string, SkillResourceType> resource_types;
    std::error_code base_ec;
    const auto base = std::filesystem::weakly_canonical(package_directory, base_ec);
    for (std::size_t i = 0; i < manifest.resources.size(); ++i) {
        const auto& resource = manifest.resources[i];
        const std::string location = "/resources/" + std::to_string(i);
        const bool artifact = resource.kind == SkillResourceType::Asset ||
                              resource.kind == SkillResourceType::Model;
        if (resource.kind == SkillResourceType::Unknown)
            issue(out, true, "unknown_resource_type", location + "/kind", "unknown resource type");
        if (!safe_id(resource.id))
            issue(out, true, "invalid_resource_id", location + "/id", "resource id is invalid");
        else if (!resource_ids.insert(resource.id).second)
            issue(out, true, "duplicate_resource_id", location + "/id", "resource id must be unique: " + resource.id);
        else resource_types.emplace(resource.id, resource.kind);
        if(resource.read_mode == SkillResourceReadMode::Unknown)
            issue(out, true, "invalid_resource_read_mode", location + "/read-mode",
                  "read-mode must be auto, text, binary, stream or mmap");
        if(resource.cache_policy == SkillCachePolicy::Unknown)
            issue(out, true, "invalid_resource_cache_policy", location + "/cache-policy",
                  "cache-policy must be no-store, on-demand or pin");
        if(resource.read_mode == SkillResourceReadMode::Text &&
           !resource.media_type.empty() && !textual_media_type(resource.media_type))
            issue(out, true, "resource_read_mode_media_type_mismatch",
                  location + "/read-mode", "text read-mode requires a textual media type");
        if(!manifest.legacy_v0 && artifact) {
            if(resource.sha256.empty())
                issue(out, true, "resource_digest_required", location + "/sha256",
                      "Manifest v1 Asset and Model resources require SHA-256");
            if(!resource.declared_size)
                issue(out, true, "resource_size_required", location + "/size",
                      "Manifest v1 Asset and Model resources require exact size");
            if(resource.license.empty())
                issue(out, true, "resource_license_required", location + "/license",
                      "Manifest v1 Asset and Model resources require license metadata");
            if(resource.source_uri.empty())
                issue(out, true, "resource_source_required", location + "/source",
                      "Manifest v1 Asset and Model resources require source metadata");
            else if(!valid_source_uri(resource.source_uri))
                issue(out, true, "invalid_resource_source", location + "/source",
                      "resource source must be an absolute URI with a scheme");
        }
        if(!resource.license.empty() &&
           (resource.license.size() > 256U || resource.license.find_first_of("\r\n") != std::string::npos))
            issue(out, true, "invalid_resource_license", location + "/license",
                  "resource license must be a single line of at most 256 bytes");
        if(resource.kind == SkillResourceType::Reference) {
            if(resource.citation && resource.citation->title.empty())
                issue(out, true, "reference_citation_title_required", location + "/citation/title",
                      "citation title is required when citation metadata is declared");
            if(resource.index && resource.index->kind != "lexical-v1")
                issue(out, true, "unsupported_reference_index", location + "/index/kind",
                      "Stage 7 supports only lexical-v1 indexes");
        } else {
            if(resource.citation)
                issue(out, true, "resource_citation_kind_mismatch", location + "/citation",
                      "citation metadata is valid only for Reference resources");
            if(resource.index)
                issue(out, true, "resource_index_kind_mismatch", location + "/index",
                      "index metadata is valid only for Reference resources");
        }
        if(resource.kind == SkillResourceType::Model && !manifest.legacy_v0) {
            if(resource.executable)
                issue(out, true, "model_executable_forbidden", location + "/executable",
                      "Model resources must not be executable");
            if(resource.runtime.empty())
                issue(out, true, "model_runtime_required", location + "/runtime",
                      "Manifest v1 Model resources require a runtime identifier");
            if(!resource.model_requirements)
                issue(out, true, "model_requirements_required", location + "/requirements",
                      "Manifest v1 Model resources require host requirements");
            else {
                if(resource.model_requirements->devices.empty())
                    issue(out, true, "model_devices_required", location + "/requirements/devices",
                          "Model resources require at least one device");
                if(resource.model_requirements->precisions.empty())
                    issue(out, true, "model_precisions_required", location + "/requirements/precisions",
                          "Model resources require at least one precision");
            }
        } else if(resource.model_requirements) {
            issue(out, true, "model_requirements_kind_mismatch", location + "/requirements",
                  "model requirements are valid only for Model resources");
        }
        if (!safe_relative(resource.path)) {
            issue(out, true, "invalid_resource_path", location + "/path", "resource path must be jail-relative without ..");
            continue;
        }
        std::error_code ec;
        const auto target = std::filesystem::weakly_canonical(package_directory / resource.path, ec);
        if (ec || !std::filesystem::exists(target)) {
            if (!resource.optional)
                issue(out, true, "resource_missing", location + "/path", "required resource does not exist: " + resource.path);
            continue;
        }
        const auto relative = std::filesystem::relative(target, base, ec);
        if (base_ec || ec || relative.empty() || *relative.begin() == "..") {
            issue(out, true, "resource_outside_jail", location + "/path", "resource resolves outside package jail");
            continue;
        }
        if (!std::filesystem::is_regular_file(target, ec)) {
            issue(out, true, "resource_not_regular", location + "/path", "resource must be a regular file");
            continue;
        }
        const auto size = std::filesystem::file_size(target, ec);
        if(!ec && resource.declared_size && size != *resource.declared_size)
            issue(out, true, "resource_size_mismatch", location + "/size",
                  "resource size does not match the exact declared size");
        if (!ec && resource.size_limit && size > *resource.size_limit)
            issue(out, true, "resource_size_exceeded", location + "/size-limit", "resource exceeds declared size limit");
        if (!resource.sha256.empty()) {
            if (!valid_sha256(resource.sha256))
                issue(out, true, "invalid_resource_sha256", location + "/sha256", "sha256 must contain 64 hexadecimal characters");
            else {
                std::string expected = resource.sha256;
                std::transform(expected.begin(), expected.end(), expected.begin(), [](unsigned char c){ return std::tolower(c); });
                const std::string actual = file_sha256(target);
                if (actual.empty())
                    issue(out, true, "resource_hash_unavailable", location + "/sha256",
                          "SHA-256 validation requires an OpenSSL-enabled build");
                else if (actual != expected)
                    issue(out, true, "resource_hash_mismatch", location + "/sha256", "resource SHA-256 does not match content");
            }
        }
    }

    auto validate_schema_ref = [&](const std::string& ref, const std::string& location) {
        if (ref.empty()) return;
        auto it = resource_types.find(ref);
        if (it == resource_types.end()) issue(out, true, "resource_reference_missing", location, "referenced resource id does not exist: " + ref);
        else if (it->second != SkillResourceType::Schema)
            issue(out, true, "resource_reference_type", location, "schema reference must target a schema resource: " + ref);
    };
    for (std::size_t i = 0; i < manifest.resources.size(); ++i) {
        validate_schema_ref(manifest.resources[i].input_schema, "/resources/" + std::to_string(i) + "/input-schema");
        validate_schema_ref(manifest.resources[i].output_schema, "/resources/" + std::to_string(i) + "/output-schema");
    }
    const auto validate_permission_subset = [&](const std::vector<std::string>& requested,
                                                const std::vector<std::string>& package,
                                                const std::string& location) {
        std::unordered_set<std::string> seen;
        for (const auto& value : requested) {
            if (!seen.insert(value).second)
                issue(out, true, "duplicate_resource_permission", location,
                      "resource permission is duplicated: " + value);
            if (std::find(package.begin(), package.end(), value) == package.end())
                issue(out, true, "resource_permission_exceeds_manifest", location,
                      "resource permission is not declared by the manifest: " + value,
                      "declare it at package level or remove it from the resource");
        }
    };
    for (std::size_t i = 0; i < manifest.resources.size(); ++i) {
        const auto& resource = manifest.resources[i];
        const auto base_location = "/resources/" + std::to_string(i);
        validate_permission_subset(resource.permissions.tools, manifest.permissions.tools,
                                   base_location + "/permissions/tools");
        validate_permission_subset(resource.permissions.network, manifest.permissions.network,
                                   base_location + "/permissions/network");
        validate_permission_subset(resource.permissions.environment, manifest.permissions.environment,
                                   base_location + "/permissions/env");
        validate_permission_subset(resource.permissions.filesystem_read,
                                   manifest.permissions.filesystem_read,
                                   base_location + "/permissions/filesystem/read");
        validate_permission_subset(resource.permissions.filesystem_write,
                                   manifest.permissions.filesystem_write,
                                   base_location + "/permissions/filesystem/write");
        validate_permission_subset(resource.permissions.secrets, manifest.permissions.secrets,
                                   base_location + "/permissions/secrets");
        std::unordered_set<std::string> dependencies;
        for (const auto& dependency : resource.depends_on) {
            if (!dependencies.insert(dependency).second)
                issue(out, true, "duplicate_resource_dependency", base_location + "/depends-on",
                      "resource dependency is duplicated: " + dependency);
            else if (dependency == resource.id)
                issue(out, true, "resource_dependency_self_reference", base_location + "/depends-on",
                      "resource cannot depend on itself: " + dependency);
            else if (resource_types.find(dependency) == resource_types.end())
                issue(out, true, "resource_dependency_missing", base_location + "/depends-on",
                      "resource dependency does not exist: " + dependency);
        }
    }
    for (std::size_t i = 0; i < manifest.dependencies.size(); ++i) {
        if (!safe_id(manifest.dependencies[i].name))
            issue(out, true, "invalid_dependency_name", "/dependencies/" + std::to_string(i) + "/name", "dependency name is invalid");
        if (manifest.dependencies[i].version.empty())
            issue(out, true, "missing_dependency_version", "/dependencies/" + std::to_string(i) + "/version", "dependency version range is required");
    }
    return out;
}

} // namespace agent_framework
