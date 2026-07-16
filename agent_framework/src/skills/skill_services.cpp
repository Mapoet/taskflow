/**
 * @file skill_services.cpp
 */

#include <agent/skill_services.hpp>
#include <agent/skill_runtime.hpp>

#include <cstdlib>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace agent_framework {

namespace {

void wire_resource_services(const std::shared_ptr<SkillServices>& services,
                            const std::filesystem::path& fallback_root) {
    const char* configured = std::getenv("AGENT_SKILL_CACHE_DIR");
    const auto cache_root = configured && *configured
        ? std::filesystem::path(configured) : fallback_root / ".skill-cache";
    services->resource_access = std::make_shared<SkillResourceAccess>();
    services->resource_cache = std::make_shared<SkillResourceCache>(cache_root);
    SkillReferenceLimits reference_limits;
    services->references = std::make_shared<SkillReferenceService>(
        reference_limits, cache_root / "derived");
    services->models = std::make_shared<SkillModelService>(services->resource_cache);
}

std::filesystem::path user_home_directory() {
#if defined(_WIN32)
    const char* h = std::getenv("USERPROFILE");
#else
    const char* h = std::getenv("HOME");
#endif
    if (!h || !*h) {
        return {};
    }
    return std::filesystem::path(h);
}

} // namespace

std::shared_ptr<SkillServices> SkillServices::from_env() {
    const char* d = std::getenv("AGENT_SKILLS_DIR");
    if (!d || !*d) {
        return nullptr;
    }
    try {
        auto reg = std::make_shared<SkillRegistry>(std::filesystem::path(d));
        reg->scan_or_reload();
        auto loader = std::make_shared<SkillLoader>(*reg);
        auto svc = std::make_shared<SkillServices>();
        svc->registry = std::move(reg);
        svc->loader = std::move(loader);
        svc->runtime = std::make_shared<SkillRuntime>(svc->registry, svc->loader);
        wire_resource_services(svc, std::filesystem::path(d));
        return svc;
    } catch (...) {
        return nullptr;
    }
}

std::shared_ptr<SkillServices> SkillServices::from_cursor_default_skill_roots() {
    const std::filesystem::path home = user_home_directory();
    if (home.empty()) {
        return nullptr;
    }
    std::vector<std::filesystem::path> roots;
    std::error_code ec;
    const std::filesystem::path a = home / ".cursor" / "skills";
    const std::filesystem::path b = home / ".cursor" / "skills-cursor";
    if (std::filesystem::is_directory(a, ec)) {
        roots.push_back(a);
    }
    if (std::filesystem::is_directory(b, ec)) {
        roots.push_back(b);
    }
    if (roots.empty()) {
        return nullptr;
    }
    try {
        auto reg = std::make_shared<SkillRegistry>(std::move(roots));
        reg->scan_or_reload();
        auto loader = std::make_shared<SkillLoader>(*reg);
        auto svc = std::make_shared<SkillServices>();
        svc->registry = std::move(reg);
        svc->loader = std::move(loader);
        svc->runtime = std::make_shared<SkillRuntime>(svc->registry, svc->loader);
        wire_resource_services(svc, home / ".cursor");
        return svc;
    } catch (...) {
        return nullptr;
    }
}

std::size_t skill_context_max_chars_from_env() {
    const char* e = std::getenv("AGENT_SKILL_CONTEXT_MAX_CHARS");
    if (!e || !*e) {
        return 8000;
    }
    const long v = std::strtol(e, nullptr, 10);
    if (v <= 0) {
        return 8000;
    }
    if (static_cast<unsigned long>(v) > std::numeric_limits<std::size_t>::max()) {
        return std::numeric_limits<std::size_t>::max() / 4;
    }
    return static_cast<std::size_t>(v);
}

std::string format_skill_catalog_l1(const SkillRegistry& registry, std::size_t max_chars) {
    const auto& ent = registry.entries();
    if (ent.empty() || max_chars == 0) {
        return {};
    }
    std::ostringstream oss;
    oss << "\n\n## Indexed skills (canonical id for run_skill_script)\n";
    constexpr std::size_t k_desc_cap = 200;
    for (const auto& e : ent) {
        std::string line = "- ";
        line += e.id;
        line += ": ";
        std::string d = e.description;
        for (char& c : d) {
            if (c == '\n' || c == '\r') {
                c = ' ';
            }
        }
        if (d.size() > k_desc_cap) {
            d.resize(k_desc_cap);
        }
        line += d;
        line += '\n';
        if (oss.tellp() + static_cast<std::streamoff>(line.size()) > static_cast<std::streamoff>(max_chars)) {
            break;
        }
        oss << line;
    }
    return oss.str();
}

} // namespace agent_framework
