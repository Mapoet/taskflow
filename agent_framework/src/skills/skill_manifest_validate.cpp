#include <agent/skill_manifest.hpp>

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
        if (resource.kind == SkillResourceType::Unknown)
            issue(out, true, "unknown_resource_type", location + "/kind", "unknown resource type");
        if (!safe_id(resource.id))
            issue(out, true, "invalid_resource_id", location + "/id", "resource id is invalid");
        else if (!resource_ids.insert(resource.id).second)
            issue(out, true, "duplicate_resource_id", location + "/id", "resource id must be unique: " + resource.id);
        else resource_types.emplace(resource.id, resource.kind);
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
    for (std::size_t i = 0; i < manifest.dependencies.size(); ++i) {
        if (!safe_id(manifest.dependencies[i].name))
            issue(out, true, "invalid_dependency_name", "/dependencies/" + std::to_string(i) + "/name", "dependency name is invalid");
        if (manifest.dependencies[i].version.empty())
            issue(out, true, "missing_dependency_version", "/dependencies/" + std::to_string(i) + "/version", "dependency version range is required");
    }
    return out;
}

} // namespace agent_framework
