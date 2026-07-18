#include <agent/resources/session_resource_context.hpp>

#include <stdexcept>

namespace agent_framework {
namespace fs = std::filesystem;
namespace {

bool within(const fs::path& root, const fs::path& candidate) {
    auto r = root.begin();
    auto c = candidate.begin();
    for (; r != root.end(); ++r, ++c) {
        if (c == candidate.end() || *r != *c) return false;
    }
    return true;
}

fs::path normalized_root(const fs::path& path) {
    if (path.empty()) return {};
    std::error_code ec;
    auto result = fs::weakly_canonical(path, ec);
    if (ec) throw std::invalid_argument("resource root cannot be canonicalized: " + path.string());
    return result;
}

} // namespace

SessionResourceContext::SessionResourceContext(fs::path workspace_root,
                                               std::vector<fs::path> skill_roots,
                                               fs::path authoring_root,
                                               fs::path cache_root,
                                               SkillRegistrySnapshot snapshot)
    : workspace_root_(normalized_root(workspace_root)), authoring_root_(normalized_root(authoring_root)),
      cache_root_(normalized_root(cache_root)), snapshot_(std::move(snapshot)) {
    for (auto& root : skill_roots) skill_roots_.push_back(normalized_root(root));
}

fs::path SessionResourceContext::jailed(const fs::path& root, const fs::path& relative) {
    if (root.empty()) throw std::runtime_error("resource domain is not configured");
    std::error_code ec;
    auto candidate = fs::weakly_canonical(root / relative, ec);
    if (ec || !within(root, candidate)) throw std::runtime_error("resource path escapes its domain");
    return candidate;
}

fs::path SessionResourceContext::resolve_local(const ResourceUri& uri) const {
    if (uri.scheme() == ResourceScheme::Workspace) return jailed(workspace_root_, uri.path());
    if (uri.scheme() == ResourceScheme::SkillCache)
        return jailed(cache_root_, fs::path(uri.authority()) / uri.path());
    if (uri.scheme() == ResourceScheme::Mcp)
        throw std::runtime_error("MCP resources are not local filesystem paths");
    const auto entry = snapshot_.get(uri.authority());
    if (!entry) throw std::runtime_error("unknown Skill resource authority: " + uri.authority());
    const fs::path root = entry->script_jail.value_or(entry->file_path.parent_path());
    return jailed(normalized_root(root), uri.path());
}

bool SessionResourceContext::mcp_allowed(std::string_view service) const {
    return allowed_mcp_services_.empty() || allowed_mcp_services_.count(std::string(service)) != 0;
}

void SessionResourceContext::set_allowed_mcp_services(std::unordered_set<std::string> services) {
    allowed_mcp_services_ = std::move(services);
}

std::vector<ResourceDomainStatus> SessionResourceContext::domains() const {
    std::vector<ResourceDomainStatus> out{{"workspace://", workspace_root_, true}};
    for (const auto& root : skill_roots_) out.push_back({"skill://", root, false});
    if (!cache_root_.empty()) out.push_back({"skill-cache://", cache_root_, true});
    if (!authoring_root_.empty()) out.push_back({"skill-authoring://", authoring_root_, true});
    return out;
}

} // namespace agent_framework
