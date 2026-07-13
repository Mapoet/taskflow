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
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

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
    assert(fs::create_directories(base / "run" / "references"));
    write_file(base / "run" / "SKILL.md",
               "---\nid: run\nscripts:\n  - hello.sh\n  - wait.sh\n  - noisy.sh\n"
               "references:\n  - references/info.md\n---\n");
    write_file(base / "run" / "hello.sh",
               "#!/bin/sh\nprintf '%s:%s:%s\\n' \"$1\" \"$2\" \"${AGENT_TEST_SECRET-unset}\"\n");
    write_file(base / "run" / "wait.sh", "#!/bin/sh\nwhile :; do sleep 1; done\n");
    write_file(base / "run" / "noisy.sh", "#!/bin/sh\nprintf '0123456789abcdefghijklmnopqrstuvwxyz'\n");
    write_file(base / "run" / "references" / "info.md", "resource-info");

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
        (void)::setenv("AGENT_TEST_SECRET", "must-not-leak", 1);
        json r = bus.call_tool(
                         "run_skill_script",
                         json{{"skill_id", "run"}, {"relative_path", "hello.sh"},
                              {"args", json::array({"alpha", "beta"})}})
                     .get();
        assert(r.contains("exit_code"));
        assert(r.at("exit_code").get<int>() == 0);
        assert(r.at("stdout").get<std::string>().find("alpha:beta:unset") != std::string::npos);
        (void)::unsetenv("AGENT_TEST_SECRET");
    }

    {
        json r = bus.call_tool(
                         "read_skill_resource",
                         json{{"skill_id", "run"}, {"relative_path", "references/info.md"},
                              {"kind", "reference"}})
                     .get();
        assert(r.value("content", "") == "resource-info");
        json denied = bus.call_tool(
                              "read_skill_resource",
                              json{{"skill_id", "run"}, {"relative_path", "hello.sh"},
                                   {"kind", "reference"}})
                          .get();
        assert(denied.contains("error"));
    }

    {
        (void)::setenv("AGENT_SKILL_SCRIPT_OUTPUT_MAX_BYTES", "16", 1);
        json r = bus.call_tool("run_skill_script",
                               json{{"skill_id", "run"}, {"relative_path", "noisy.sh"}})
                     .get();
        assert(r.value("truncated", false));
        assert(r.at("stdout").get<std::string>().size() == 16U);
        (void)::unsetenv("AGENT_SKILL_SCRIPT_OUTPUT_MAX_BYTES");
    }

    {
        std::atomic<bool> cancelled{false};
        ToolCallControl control{[&] { return cancelled.load(); }};
        auto future = bus.call_tool(
            "run_skill_script", json{{"skill_id", "run"}, {"relative_path", "wait.sh"}}, control);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancelled.store(true);
        json r = future.get();
        assert(r.value("cancelled", false));
        assert(r.value("exit_code", 0) == -1);
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
