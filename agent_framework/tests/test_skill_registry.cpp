/**
 * @file test_skill_registry.cpp
 * @brief WP1.8 SkillRegistry / SkillLoader 单测（无网络）
 */

#include <agent/skill_loader.hpp>
#include <agent/skill_registry.hpp>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

namespace fs = std::filesystem;

void write_file(const fs::path& p, const std::string& content) {
    std::ofstream f(p);
    assert(f.good());
    f << content;
}

} // namespace

int main() {
    using namespace agent_framework;

    const fs::path base = fs::temp_directory_path() / "agent_skill_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    assert(fs::create_directories(base));

    // --- missing id → skip ---
    write_file(
        base / "bad.skill.md",
        std::string("---\nname: no_id\n---\nbody\n"));
    {
        SkillRegistry reg(base);
        reg.scan_or_reload();
        assert(reg.entries().empty());
    }
    fs::remove(base / "bad.skill.md");

    // --- duplicate id: second file skipped ---
    fs::create_directories(base / "a");
    fs::create_directories(base / "b");
    write_file(base / "a" / "first.skill.md",
               std::string("---\nid: dup\n---\nalpha\n"));
    write_file(base / "b" / "second.skill.md",
               std::string("---\nid: dup\n---\nbeta\n"));
    {
        SkillRegistry reg(base);
        reg.scan_or_reload();
        assert(reg.entries().size() == 1U);
        assert(reg.get("dup").has_value());
    }
    fs::remove_all(base / "a", ec);
    fs::remove_all(base / "b", ec);

    // --- match + loader + in-body --- ---
    write_file(base / "route.skill.md",
               std::string("---\nid: route\n"
                           "trigger_keywords:\n"
                           "  - unique_kw_xyz\n"
                           "---\n"
                           "line1\n"
                           "---\n"
                           "line2\n"));
    {
        SkillRegistry reg(base);
        reg.scan_or_reload();
        const auto m = reg.match("hello unique_kw_xyz world");
        assert(m.has_value());
        assert(*m == "route");
        SkillLoader loader(reg);
        const auto body = loader.load_instructions("route", 10000);
        assert(body.has_value());
        assert(body->find("line1") != std::string::npos);
        assert(body->find("line2") != std::string::npos);
        const auto tiny = loader.load_instructions("route", 4);
        assert(tiny.has_value());
        assert(tiny->size() <= 4U);
    }
    fs::remove(base / "route.skill.md");

    // --- merge two scan roots ---
    fs::create_directories(base / "r1");
    fs::create_directories(base / "r2");
    write_file(base / "r1" / "one.skill.md",
               std::string("---\nid: one\n---\n"));
    write_file(base / "r2" / "two.skill.md",
               std::string("---\nid: two\n---\n"));
    {
        SkillRegistry reg({base / "r1", base / "r2"});
        reg.scan_or_reload();
        assert(reg.entries().size() == 2U);
        assert(reg.get("one").has_value());
        assert(reg.get("two").has_value());
    }
    fs::remove_all(base / "r1", ec);
    fs::remove_all(base / "r2", ec);

    // --- router off ---
    write_file(base / "off.skill.md",
               std::string("---\nid: off\n"
                           "trigger_keywords:\n"
                           "  - marker_only\n"
                           "---\n"));
    {
#if defined(_WIN32)
        (void)_putenv_s("AGENT_SKILL_ROUTER", "off");
#else
        (void)::setenv("AGENT_SKILL_ROUTER", "off", 1);
#endif
        SkillRegistry reg(base);
        reg.scan_or_reload();
        assert(!reg.match("marker_only").has_value());
#if defined(_WIN32)
        (void)_putenv_s("AGENT_SKILL_ROUTER", "");
#else
        (void)::unsetenv("AGENT_SKILL_ROUTER");
#endif
    }
    fs::remove(base / "off.skill.md");

    fs::remove_all(base, ec);
    std::clog << "test_skill_registry: ok\n";
    return 0;
}
