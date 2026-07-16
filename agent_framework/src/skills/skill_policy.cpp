#include <agent/skill_policy.hpp>

#include <algorithm>
#include <cctype>
#include <system_error>

namespace agent_framework {

bool skill_permissions_empty(const SkillPermissionSet& permissions) noexcept {
    return permissions.tools.empty() && permissions.network.empty() &&
           permissions.environment.empty() && permissions.filesystem_read.empty() &&
           permissions.filesystem_write.empty() && permissions.secrets.empty();
}

SkillPermissionSet skill_permissions_effective(const SkillPermissionSet& manifest,
                                               const SkillPermissionSet& resource) {
    return skill_permissions_empty(resource) ? manifest : resource;
}
namespace {

bool exact_or_all(const std::vector<std::string>& values, std::string_view target) {
    return std::any_of(values.begin(), values.end(), [&](const std::string& value) {
        return value == "*" || value == target;
    });
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string normalize_origin(std::string_view raw) {
    std::string value(raw);
    const auto scheme_pos = value.find("://");
    if (scheme_pos == std::string::npos) return lower(value);
    std::string scheme = lower(value.substr(0, scheme_pos));
    std::string authority = value.substr(scheme_pos + 3);
    const auto end = authority.find_first_of("/?#");
    if (end != std::string::npos) authority.resize(end);
    const auto at = authority.rfind('@');
    if (at != std::string::npos) authority.erase(0, at + 1);
    authority = lower(authority);
    if ((scheme == "https" && authority.size() > 4 && authority.ends_with(":443")) ||
        (scheme == "http" && authority.size() > 3 && authority.ends_with(":80"))) {
        authority.resize(authority.rfind(':'));
    }
    return scheme + "://" + authority;
}

bool origin_matches(const std::string& scope_raw, const std::string& target) {
    if (scope_raw == "*") return true;
    const std::string scope = normalize_origin(scope_raw);
    if (scope == target) return true;
    const auto marker = scope.find("://*.");
    if (marker == std::string::npos) return false;
    const std::string prefix = scope.substr(0, marker + 3);
    const std::string suffix = scope.substr(marker + 5);
    if (!target.starts_with(prefix)) return false;
    const std::string host = target.substr(prefix.size());
    return host.size() > suffix.size() + 1 && host.ends_with("." + suffix);
}

bool any_origin(const std::vector<std::string>& scopes, const std::string& target) {
    return std::any_of(scopes.begin(), scopes.end(), [&](const std::string& scope) {
        return origin_matches(scope, target);
    });
}

std::filesystem::path canonical_for_policy(const std::filesystem::path& path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) return canonical.lexically_normal();
    return std::filesystem::absolute(path, ec).lexically_normal();
}

bool path_contains(const std::filesystem::path& scope, const std::filesystem::path& target) {
    const auto relative = target.lexically_relative(scope);
    if (relative.empty()) return target == scope;
    const auto first = relative.begin();
    return first != relative.end() && *first != "..";
}

std::filesystem::path resolve_scope(const std::filesystem::path& root, const std::string& value) {
    const std::filesystem::path scope(value);
    return canonical_for_policy(scope.is_absolute() ? scope : root / scope);
}

SkillPolicyDecision decision(bool allowed, SkillPermissionKind kind, std::string action,
                             std::string target, std::string reason) {
    return {allowed, kind, std::move(action), std::move(target), std::move(reason)};
}

} // namespace

SkillPolicyEngine::SkillPolicyEngine(SkillPermissionSet requested, SkillPermissionGrant granted,
                                     std::filesystem::path package_root)
    : requested_(std::move(requested)), granted_(std::move(granted)),
      package_root_(canonical_for_policy(package_root.empty() ? std::filesystem::current_path()
                                                              : package_root)) {}

SkillPolicyDecision SkillPolicyEngine::authorize_tool(std::string_view name) const {
    const bool allowed = exact_or_all(requested_.tools, name) && exact_or_all(granted_.tools, name);
    return decision(allowed, SkillPermissionKind::Tool, "invoke", std::string(name),
                    allowed ? "manifest request intersects task grant"
                            : "tool is not present in both manifest permissions and task grants");
}

SkillPolicyDecision SkillPolicyEngine::authorize_network(std::string_view raw) const {
    const std::string origin = normalize_origin(raw);
    const bool allowed = !origin.empty() && any_origin(requested_.network, origin) &&
                         any_origin(granted_.network, origin);
    return decision(allowed, SkillPermissionKind::Network, "connect", origin,
                    allowed ? "origin allowed" : "origin is not present in both permission sets");
}

SkillPolicyDecision SkillPolicyEngine::authorize_environment(std::string_view variable) const {
    const bool allowed = exact_or_all(requested_.environment, variable) &&
                         exact_or_all(granted_.environment, variable);
    return decision(allowed, SkillPermissionKind::Environment, "read", std::string(variable),
                    allowed ? "environment reference allowed" : "environment reference denied");
}

SkillPolicyDecision SkillPolicyEngine::authorize_filesystem(const std::filesystem::path& raw,
                                                             bool write) const {
    const auto target = canonical_for_policy(raw.is_absolute() ? raw : package_root_ / raw);
    const auto& requested = write ? requested_.filesystem_write : requested_.filesystem_read;
    const auto& granted = write ? granted_.filesystem_write : granted_.filesystem_read;
    const auto matches = [&](const std::vector<std::string>& scopes) {
        return std::any_of(scopes.begin(), scopes.end(), [&](const std::string& scope) {
            if (scope == "*") return true;
            return path_contains(resolve_scope(package_root_, scope), target);
        });
    };
    const bool allowed = matches(requested) && matches(granted);
    return decision(allowed, write ? SkillPermissionKind::FilesystemWrite
                                   : SkillPermissionKind::FilesystemRead,
                    write ? "write" : "read", target.string(),
                    allowed ? "path allowed" : "path is not contained by both permission sets");
}

SkillPolicyDecision SkillPolicyEngine::authorize_secret(std::string_view reference) const {
    const bool allowed = exact_or_all(requested_.secrets, reference) &&
                         exact_or_all(granted_.secrets, reference);
    return decision(allowed, SkillPermissionKind::Secret, "resolve", std::string(reference),
                    allowed ? "secret reference allowed" : "secret reference denied");
}

const char* skill_permission_kind_cstr(SkillPermissionKind kind) noexcept {
    switch (kind) {
    case SkillPermissionKind::Tool: return "tool";
    case SkillPermissionKind::Network: return "network";
    case SkillPermissionKind::Environment: return "environment";
    case SkillPermissionKind::FilesystemRead: return "filesystem_read";
    case SkillPermissionKind::FilesystemWrite: return "filesystem_write";
    case SkillPermissionKind::Secret: return "secret";
    }
    return "unknown";
}

} // namespace agent_framework
