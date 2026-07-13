/**
 * @file test_skill_registry.cpp
 * @brief WP1.8 SkillRegistry / SkillLoader 单测（无网络）
 *
 * 布局：`<root>/<skill-folder>/SKILL.md`
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

    // --- no YAML frontmatter → skip ---
    fs::create_directories(base / "raw");
    write_file(base / "raw" / "SKILL.md", std::string("just markdown with no frontmatter\n"));
    {
        SkillRegistry reg(base);
        reg.scan_or_reload();
        assert(reg.entries().empty());
    }
    fs::remove_all(base / "raw", ec);

    // --- canonical from directory when name/id absent ---
    fs::create_directories(base / "foldonly");
    write_file(
        base / "foldonly" / "SKILL.md",
        std::string("---\ndescription: folddesc\n---\nbody\n"));
    {
        SkillRegistry reg(base);
        reg.scan_or_reload();
        assert(reg.entries().size() == 1U);
        assert(reg.get("foldonly").has_value());
        assert(reg.get("foldonly")->description == "folddesc");
    }
    fs::remove_all(base / "foldonly", ec);

    // --- Cursor-style description block + name ---
    fs::create_directories(base / "cur");
    write_file(base / "cur" / "SKILL.md",
               std::string("---\nname: cur\n"
                           "description: >-\n"
                           "  hello merge-ready tail\n"
                           "---\n# H\n"));
    {
        SkillRegistry reg(base);
        reg.scan_or_reload();
        const auto e = reg.get("cur");
        assert(e.has_value());
        assert(e->description.find("merge-ready") != std::string::npos);
    }
    fs::remove_all(base / "cur", ec);

    // --- match by canonical / name substring (no trigger_keywords) ---
    fs::create_directories(base / "byname");
    write_file(
        base / "byname" / "SKILL.md",
        std::string("---\nname: unique_skill_alpha\n---\n"));
    {
        SkillRegistry reg(base);
        reg.scan_or_reload();
        const auto m = reg.match("please use unique_skill_alpha thanks");
        assert(m.has_value());
        assert(*m == "unique_skill_alpha");
    }
    fs::remove_all(base / "byname", ec);

    // --- duplicate canonical: second package skipped ---
    fs::create_directories(base / "a");
    fs::create_directories(base / "b");
    write_file(base / "a" / "SKILL.md",
               std::string("---\nid: dup\n---\nalpha\n"));
    write_file(base / "b" / "SKILL.md",
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
    fs::create_directories(base / "route");
    write_file(base / "route" / "SKILL.md",
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
    fs::remove_all(base / "route", ec);

    // --- typed metadata, declared resources, limits, and diagnostics ---
    fs::create_directories(base / "managed" / "references");
    fs::create_directories(base / "managed" / "scripts");
    write_file(base / "managed" / "SKILL.md",
               "---\nname: managed\ndescription: controlled resources\nversion: 1.2.3\n"
               "license: Apache-2.0\nscripts:\n  - scripts/run.sh\nreferences:\n"
               "  - references/guide.md\ncli:\n  - cli/helper\nallowed-tools:\n"
               "  - read_file\n---\nbody\n");
    write_file(base / "managed" / "references" / "guide.md", "guide");
    write_file(base / "managed" / "scripts" / "run.sh", "echo run\n");
    {
        SkillRegistry reg(base);
        reg.scan_or_reload();
        assert(reg.valid());
        const auto entry = reg.get("managed");
        assert(entry && entry->version == "1.2.3" && entry->license == "Apache-2.0");
        assert(entry->scripts.size() == 1U && entry->references.size() == 1U);
        assert(entry->cli_programs.size() == 1U && entry->allowed_tools.size() == 1U);
        SkillLoader loader(reg);
        std::string error;
        const auto guide = loader.load_resource(
            "managed", "references/guide.md", SkillResourceKind::Reference, 16, &error);
        assert(guide && *guide == "guide");
        assert(!loader.load_resource(
            "managed", "scripts/run.sh", SkillResourceKind::Reference, 16, &error));
        assert(!loader.load_resource(
            "managed", "references/guide.md", SkillResourceKind::Reference, 2, &error));
        assert(!loader.load_resource(
            "managed", "../SKILL.md", SkillResourceKind::AnyDeclared, 16, &error));
    }
    fs::remove_all(base / "managed", ec);

    fs::create_directories(base / "invalid");
    write_file(base / "invalid" / "SKILL.md",
               "---\nname: bad/id\nscripts:\n  - ../escape.sh\n---\n");
    {
        SkillRegistry reg(base);
        reg.scan_or_reload();
        assert(!reg.valid());
        assert(!reg.get("bad/id"));
        bool invalid_id = false;
        for (const auto& d : reg.diagnostics()) invalid_id |= d.code == "invalid_skill_id";
        assert(invalid_id);
    }
    fs::remove_all(base / "invalid", ec);

    // Legacy packages without declarations remain restricted to kind directories.
    fs::create_directories(base / "legacy" / "references");
    write_file(base / "legacy" / "SKILL.md", "---\nid: legacy\n---\n");
    write_file(base / "legacy" / "references" / "note.md", "legacy-note");
    write_file(base / "legacy" / "other.txt", "not-authorized");
    {
        SkillRegistry reg(base);
        reg.scan_or_reload();
        SkillLoader loader(reg);
        std::string error;
        assert(loader.load_resource(
            "legacy", "references/note.md", SkillResourceKind::Reference, 64, &error));
        assert(!loader.load_resource(
            "legacy", "other.txt", SkillResourceKind::Reference, 64, &error));
    }
    fs::remove_all(base / "legacy", ec);

    // --- merge two scan roots ---
    fs::create_directories(base / "r1" / "one");
    fs::create_directories(base / "r2" / "two");
    write_file(base / "r1" / "one" / "SKILL.md",
               std::string("---\nid: one\n---\n"));
    write_file(base / "r2" / "two" / "SKILL.md",
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
    fs::create_directories(base / "off");
    write_file(base / "off" / "SKILL.md",
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
    fs::remove_all(base / "off", ec);

    fs::remove_all(base, ec);
    std::clog << "test_skill_registry: ok\n";
    return 0;
}
