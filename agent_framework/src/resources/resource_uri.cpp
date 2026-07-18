#include <agent/resources/resource_uri.hpp>

#include <cctype>
#include <stdexcept>
#include <vector>

namespace agent_framework {
namespace {

bool valid_authority(std::string_view value) {
    if (value.empty()) return false;
    for (unsigned char c : value) {
        if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.')) return false;
    }
    return value != "." && value != "..";
}

std::string normalize_path(std::string_view raw) {
    if (raw.empty() || raw.front() == '/' || raw.front() == '\\')
        throw std::invalid_argument("resource path must be non-empty and relative");
    std::string out;
    std::size_t start = 0;
    while (start <= raw.size()) {
        const auto end = raw.find('/', start);
        const auto part = raw.substr(start, end == std::string_view::npos ? raw.size() - start
                                                                          : end - start);
        if (part.empty() || part == "." || part == ".." || part.find('\\') != std::string_view::npos)
            throw std::invalid_argument("resource path contains an unsafe segment");
        if (!out.empty()) out.push_back('/');
        out.append(part);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return out;
}

std::string opaque_mcp_identifier(std::string_view raw) {
    if (raw.empty()) throw std::invalid_argument("MCP resource identifier must be non-empty");
    for (unsigned char c : raw) {
        if (c < 0x20U || c == 0x7fU)
            throw std::invalid_argument("MCP resource identifier contains a control character");
    }
    return std::string(raw);
}

} // namespace

ResourceUri ResourceUri::parse(std::string_view value) {
    const auto split = value.find("://");
    if (split == std::string_view::npos) throw std::invalid_argument("resource URI requires ://");
    const auto scheme = value.substr(0, split);
    auto rest = value.substr(split + 3);
    ResourceUri uri;
    if (scheme == "workspace") {
        uri.scheme_ = ResourceScheme::Workspace;
        uri.path_ = normalize_path(rest);
        return uri;
    }
    if (scheme == "skill") uri.scheme_ = ResourceScheme::Skill;
    else if (scheme == "skill-cache") uri.scheme_ = ResourceScheme::SkillCache;
    else if (scheme == "mcp") uri.scheme_ = ResourceScheme::Mcp;
    else throw std::invalid_argument("unsupported resource URI scheme");

    const auto slash = rest.find('/');
    if (slash == std::string_view::npos) throw std::invalid_argument("resource URI requires authority/path");
    uri.authority_ = std::string(rest.substr(0, slash));
    if (!valid_authority(uri.authority_)) throw std::invalid_argument("invalid resource URI authority");
    if (uri.scheme_ == ResourceScheme::Mcp)
        uri.path_ = opaque_mcp_identifier(rest.substr(slash + 1));
    else
        uri.path_ = normalize_path(rest.substr(slash + 1));
    return uri;
}

const char* resource_scheme_name(ResourceScheme scheme) noexcept {
    switch (scheme) {
        case ResourceScheme::Workspace: return "workspace";
        case ResourceScheme::Skill: return "skill";
        case ResourceScheme::SkillCache: return "skill-cache";
        case ResourceScheme::Mcp: return "mcp";
    }
    return "unknown";
}

std::string ResourceUri::str() const {
    std::string out = std::string(resource_scheme_name(scheme_)) + "://";
    if (!authority_.empty()) out += authority_ + "/";
    return out + path_;
}

} // namespace agent_framework
