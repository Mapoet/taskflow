/**
 * @file skill_services.cpp
 */

#include <agent/skill_services.hpp>

#include <cstdlib>
#include <filesystem>
#include <limits>
#include <vector>

namespace agent_framework {

namespace {

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

} // namespace agent_framework
