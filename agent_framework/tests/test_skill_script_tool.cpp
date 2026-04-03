/**
 * @file test_skill_script_tool.cpp
 * @brief WP1.8 run_skill_script：校验、allowlist、POSIX 执行（无网络）
 *
 * CTest 设置 `AGENT_SKILL_SCRIPT_ALLOWLIST=/bin/sh`（见 CMakeLists）。
 */

#include <agent/skill_registry.hpp>
#include <agent/skill_script_tool.hpp>
#include <agent/skill_services.hpp>
#include <agent/toolbus.hpp>

#include <cassert>
#include <cstdlib>
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
#if defined(_WIN32)
    std::clog << "test_skill_script_tool: skip (POSIX-only execution path)\n";
    return 0;
#else
    using namespace agent_framework;
    using json = nlohmann::json;

    const fs::path base = fs::temp_directory_path() / "agent_skill_script_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    assert(fs::create_directories(base));
    assert(fs::create_directories(base / "run"));
    write_file(base / "run" / "SKILL.md",
               std::string("---\nid: run\n---\n"));
    write_file(base / "run" / "hello.sh", std::string("#!/bin/sh\necho ok\n"));

    auto reg = std::make_shared<SkillRegistry>(base);
    reg->scan_or_reload();
    auto loader = std::make_shared<SkillLoader>(*reg);
    auto svc = std::make_shared<SkillServices>();
    svc->registry = reg;
    svc->loader = loader;

    ToolBus bus;
    register_skill_script_tool(bus, svc);
    register_skill_script_tool(bus, svc); // idempotent (same ToolBus, e.g. REPL rebuild)

    {
        json r = bus.call_tool("run_skill_script",
                               json{{"skill_id", "run"}, {"relative_path", "hello.sh"}})
                     .get();
        assert(r.contains("exit_code"));
        assert(r.at("exit_code").get<int>() == 0);
        assert(r.at("stdout").get<std::string>().find("ok") != std::string::npos);
    }

    {
        json r = bus.call_tool("run_skill_script",
                               json{{"skill_id", "run"}, {"relative_path", "../SKILL.md"}})
                     .get();
        assert(r.contains("error"));
        assert(r["error"].at("code") == "validation_failed");
    }

#if !defined(_WIN32)
    {
        const fs::path outside = base / "outside";
        assert(fs::create_directories(outside));
        write_file(outside / "secret.txt", "x");
        std::error_code e2;
        fs::create_directory_symlink(outside, base / "run" / "sym", e2);
        if (!e2) {
            json r = bus.call_tool("run_skill_script",
                                   json{{"skill_id", "run"}, {"relative_path", "sym/secret.txt"}})
                         .get();
            assert(r.contains("error"));
            assert(r["error"].at("code") == "validation_failed");
        }
    }
#endif

    {
#if defined(_WIN32)
        (void)_putenv_s("AGENT_SKILL_SCRIPT_ALLOWLIST", "");
#else
        (void)::unsetenv("AGENT_SKILL_SCRIPT_ALLOWLIST");
#endif
        ToolBus bus2;
        register_skill_script_tool(bus2, svc);
        json r = bus2.call_tool("run_skill_script",
                                json{{"skill_id", "run"}, {"relative_path", "hello.sh"}})
                     .get();
        assert(r.contains("error"));
        assert(r["error"].at("code") == "validation_failed");
#if defined(_WIN32)
        (void)_putenv_s("AGENT_SKILL_SCRIPT_ALLOWLIST", "/bin/sh");
#else
        (void)::setenv("AGENT_SKILL_SCRIPT_ALLOWLIST", "/bin/sh", 1);
#endif
    }

    fs::remove_all(base, ec);
    std::clog << "test_skill_script_tool: ok\n";
    return 0;
#endif
}
