/**
 * @file test_user_input_preprocessor.cpp
 * @brief WP2.7：UserInputPreprocessor 单测 U-1..U-8
 */
#include <agent/agent/execution_context.hpp>
#include <agent/toolbus/fs_tools.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/core/types.hpp>
#include <agent/agent/user_input_preprocessor.hpp>
#include <agent/resources/session_resource_context.hpp>
#include <agent/skills/skill_loader.hpp>
#include <agent/skills/skill_registry.hpp>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#if defined(_WIN32)
int main() {
    std::clog << "test_user_input_preprocessor: skip on Windows\n";
    return 0;
}
#else

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace agent_framework;

namespace {

void set_allowlist_empty() {
    (void)::unsetenv("AGENT_TOOL_ALLOWLIST");
}

ExecutionContext ctx() {
    ExecutionContext c = ExecutionContext::from_environment();
    c.input_policy_version = "wp27-v1";
    return c;
}

void u1_plain_text() {
    (void)::setenv("AGENT_INPUT_STRICT", "1", 1);
    PreprocessOptions opt;
    UserInputPreprocessor p(opt);
    auto o = p.process("hello  world\n\n\nline", ctx());
    assert(o.tier_a_violations.empty());
    assert(o.injected_context.empty());
    assert(o.control_actions.empty());
    assert(o.llm_user_text.find("hello") != std::string::npos);
}

void u2_file_outside_jail() {
    (void)::setenv("AGENT_INPUT_STRICT", "1", 1);
    const fs::path root = fs::temp_directory_path() / "wp27_u2_root";
    const fs::path outside = fs::temp_directory_path() / "wp27_u2_out";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove_all(outside, ec);
    assert(fs::create_directories(root));
    assert(fs::create_directories(outside));
    {
        std::ofstream f(outside / "x.txt");
        f << "secret";
    }
    (void)::setenv("AGENT_FS_ROOT", root.string().c_str(), 1);
    (void)::setenv("AGENT_FS_MAX_READ_BYTES", "65536", 1);
    (void)::setenv("AGENT_FS_MAX_WRITE_BYTES", "65536", 1);

    auto bus = std::make_shared<ToolBus>();
    register_builtin_fs_tools_if_configured(*bus);
    PreprocessOptions opt;
    opt.toolbus = bus;
    UserInputPreprocessor prep(opt);
    std::string outside_file = (outside / "x.txt").string();
    std::string raw = "x @file(" + outside_file + ") y";
    auto o = prep.process(raw, ctx());
    assert(!o.tier_a_violations.empty());
    bool found = false;
    for (const auto& v : o.tier_a_violations) {
        if (v.find("file_path_outside_jail") != std::string::npos) {
            found = true;
        }
    }
    assert(found);
}

void u3_file_mock_bus() {
    (void)::setenv("AGENT_INPUT_STRICT", "1", 1);
    const fs::path root = fs::temp_directory_path() / "wp27_u3";
    std::error_code ec;
    fs::remove_all(root, ec);
    assert(fs::create_directories(root));
    {
        std::ofstream f(root / "a.txt");
        f << "x";
    }
    (void)::setenv("AGENT_FS_ROOT", root.string().c_str(), 1);
    (void)::setenv("AGENT_FS_MAX_READ_BYTES", "65536", 1);
    (void)::setenv("AGENT_FS_MAX_WRITE_BYTES", "65536", 1);

    auto bus = std::make_shared<ToolBus>();
    ToolMeta m;
    m.name = "fs_read";
    // Must accept max_bytes (and optional mode) like builtin fs_read; else schema rejects the call.
    m.schema = json::parse(R"({
        "type": "object",
        "properties": {
            "path": {"type": "string"},
            "max_bytes": {"type": "integer"},
            "mode": {"type": "string"}
        },
        "required": ["path"]
    })");
    bus->register_local_tool(
        "fs_read",
        [](const json& j) {
            (void)j;
            return json{{"content", std::string("INJ_BODY")}};
        },
        m);
    PreprocessOptions opt;
    opt.toolbus = bus;
    UserInputPreprocessor prep(opt);
    ExecutionContext cx = ctx();
    cx.cwd = root.string();
    auto o = prep.process("pre @file(a.txt) post", cx);
    assert(o.tier_a_violations.empty());
    assert(o.injected_context.size() == 1);
    assert(o.injected_context[0].source_kind == "file");
    assert(o.injected_context[0].text_utf8 == "INJ_BODY");
}

void u4_memory_compact() {
    (void)::setenv("AGENT_INPUT_STRICT", "1", 1);
    PreprocessOptions opt;
    UserInputPreprocessor p(opt);
    auto o = p.process("hello\n/memory compact\n", ctx());
    assert(o.tier_a_violations.empty());
    assert(o.control_actions.size() == 1);
    assert(o.control_actions[0].command == "memory.compact");
}

void u5_unknown_cmd() {
    (void)::setenv("AGENT_INPUT_STRICT", "1", 1);
    PreprocessOptions opt;
    UserInputPreprocessor p(opt);
    auto o = p.process("/unknown", ctx());
    assert(o.control_actions.empty());
    assert(!o.tier_a_violations.empty());
    assert(o.tier_a_violations[0].find("command_not_whitelisted") != std::string::npos);
}

void u6_multiline_cmd_strip() {
    (void)::setenv("AGENT_INPUT_STRICT", "1", 1);
    PreprocessOptions opt;
    UserInputPreprocessor p(opt);
    auto o = p.process("line a\n  /memory compact  \nline b", ctx());
    assert(o.tier_a_violations.empty());
    assert(o.control_actions.size() == 1);
    assert(o.llm_user_text.find("line a") != std::string::npos);
    assert(o.llm_user_text.find("line b") != std::string::npos);
    assert(o.llm_user_text.find("/memory") == std::string::npos);
}

void u7_url_mock() {
    (void)::setenv("AGENT_INPUT_STRICT", "1", 1);
    ToolBus bus;
    ToolMeta m;
    m.name = "web_fetch";
    m.schema = json::parse(R"({"type":"object","properties":{"url":{"type":"string"}}})");
    bus.register_local_tool(
        "web_fetch",
        [](const json& j) {
            assert(j.at("url").get<std::string>() == "https://example.invalid/test");
            return json{{"text", std::string("PAGE")}};
        },
        m);
    PreprocessOptions opt;
    opt.toolbus = std::shared_ptr<ToolBus>(&bus, [](ToolBus*) {});
    UserInputPreprocessor prep(opt);
    auto o = prep.process("see @url(https://example.invalid/test) end", ctx());
    assert(o.tier_a_violations.empty());
    assert(o.injected_context.size() == 1);
    assert(o.injected_context[0].source_kind == "url");
    assert(o.injected_context[0].source_ref == "https://example.invalid/test");
    assert(o.injected_context[0].text_utf8 == "PAGE");
}

void u8_injection_budget() {
    (void)::setenv("AGENT_INPUT_STRICT", "1", 1);
    (void)::setenv("AGENT_BUDGET_MAX_INJECTION_BYTES", "120", 1);
    const fs::path root = fs::temp_directory_path() / "wp27_u8";
    std::error_code ec;
    fs::remove_all(root, ec);
    assert(fs::create_directories(root));
    {
        std::ofstream f(root / "a.txt");
        f << "x";
    }
    (void)::setenv("AGENT_FS_ROOT", root.string().c_str(), 1);
    (void)::setenv("AGENT_FS_MAX_READ_BYTES", "65536", 1);
    (void)::setenv("AGENT_FS_MAX_WRITE_BYTES", "65536", 1);

    auto bus = std::make_shared<ToolBus>();
    ToolMeta mf;
    mf.name = "fs_read";
    mf.schema = json::parse(R"({
        "type": "object",
        "properties": {
            "path": {"type": "string"},
            "max_bytes": {"type": "integer"},
            "mode": {"type": "string"}
        },
        "required": ["path"]
    })");
    bus->register_local_tool(
        "fs_read",
        [](const json&) { return json{{"content", std::string(50, 'z')}}; }, mf);
    ToolMeta mw;
    mw.name = "web_fetch";
    mw.schema = json::parse(R"({"type":"object","properties":{"url":{"type":"string"}}})");
    bus->register_local_tool(
        "web_fetch",
        [](const json&) { return json{{"text", std::string(50, 'a')}}; }, mw);

    PreprocessOptions opt;
    opt.toolbus = bus;
    UserInputPreprocessor prep(opt);
    ExecutionContext cx = ctx();
    cx.cwd = root.string();
    auto o = prep.process("@file(a.txt) @url(https://example.invalid/b)", cx);
    bool exceeded = false;
    for (const auto& v : o.tier_a_violations) {
        if (v.find("injection_budget_exceeded") != std::string::npos) {
            exceeded = true;
        }
    }
    assert(exceeded);
    (void)::unsetenv("AGENT_BUDGET_MAX_INJECTION_BYTES");
}

void u9_skills_commands() {
    (void)::setenv("AGENT_INPUT_STRICT", "1", 1);
    UserInputPreprocessor p(PreprocessOptions{});
    auto list = p.process("/skills list", ctx());
    assert(list.tier_a_violations.empty());
    assert(list.control_actions.size() == 1);
    assert(list.control_actions[0].command == "skills.list");
    auto activate = p.process("/skills activate example", ctx());
    assert(activate.control_actions[0].args.value("id", "") == "example");
    auto create = p.process("/skills create example --description useful research skill", ctx());
    assert(create.control_actions[0].command == "skills.create");
    assert(create.control_actions[0].args.value("description", "") == "useful research skill");
    auto invalid = p.process("/skills activate", ctx());
    assert(!invalid.tier_a_violations.empty());
}

void u10_resource_uri_injection() {
    const fs::path base = fs::temp_directory_path() / "wp27_resource_uri";
    const fs::path workspace = base / "workspace";
    const fs::path skills = base / "skills";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(workspace);
    fs::create_directories(skills / "sample/references");
    { std::ofstream(workspace / "input.md") << "WORKSPACE_RESOURCE"; }
    { std::ofstream(skills / "sample/references/data.md") << "SKILL_RESOURCE"; }
    { std::ofstream(skills / "sample/SKILL.md") << R"(---
name: sample
description: Injection fixture
references: [references/data.md]
---
Body
)"; }
    (void)::setenv("AGENT_FS_ROOT", workspace.string().c_str(), 1);
    (void)::setenv("AGENT_FS_MAX_READ_BYTES", "65536", 1);
    (void)::setenv("AGENT_FS_MAX_WRITE_BYTES", "65536", 1);
    auto registry = std::make_shared<SkillRegistry>(skills);
    registry->scan_or_reload();
    auto loader = std::make_shared<SkillLoader>(*registry);
    auto resources = std::make_shared<SessionResourceContext>(
        workspace, std::vector<fs::path>{skills}, fs::path{}, base / "cache", registry->snapshot());
    auto bus = std::make_shared<ToolBus>();
    register_builtin_fs_tools_if_configured(*bus);
    PreprocessOptions opt;
    opt.toolbus = bus;
    UserInputPreprocessor prep(opt);
    ExecutionContext context = ctx();
    context.resources = resources;
    context.skill_loader = loader;
    auto output = prep.process(
        "@{workspace://input.md} @{skill://sample/references/data.md}", context);
    assert(output.tier_a_violations.empty());
    assert(output.injected_context.size() == 2);
    assert(output.injected_context[0].text_utf8 == "WORKSPACE_RESOURCE");
    assert(output.injected_context[1].text_utf8 == "SKILL_RESOURCE");
    auto denied = prep.process("@{skill://sample/../secret}", context);
    assert(!denied.tier_a_violations.empty());
}

} // namespace

int main() {
    set_allowlist_empty();
    u1_plain_text();
    u2_file_outside_jail();
    u3_file_mock_bus();
    u4_memory_compact();
    u5_unknown_cmd();
    u6_multiline_cmd_strip();
    u7_url_mock();
    u8_injection_budget();
    u9_skills_commands();
    u10_resource_uri_injection();
    std::cout << "test_user_input_preprocessor: ok\n";
    return 0;
}

#endif
