/**
 * @file test_tool_call_hooks.cpp
 * @brief WP2.1d：ToolBus 调用前 hook（H-1–H-7）
 *
 * 运行：默认套件需 `AGENT_TOOL_ALLOWLIST=`（CTest 已设）
 *      `test_tool_call_hooks --h5`：子测试进程，allowlist 仅含占位名，验证 tool_not_allowed 不调 hook
 */

#include <agent/toolbus/toolbus.hpp>

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace agent_framework {
namespace detail {

struct ToolBusCallHookTestPeer {
    /**
     * 绕过 register_local_tool 的 allowlist，直接插入表项，用于单测 tool_not_allowed 路径。
     */
    static void emplace_tool(ToolBus& bus, const std::string& name,
                             std::function<json(const json&)> fn, const ToolMeta& meta) {
        std::lock_guard<std::mutex> lock(bus.tools_mutex_);
        bus.tools_.emplace(name, std::make_shared<LocalTool>(name, std::move(fn), meta));
    }
};

} // namespace detail
} // namespace agent_framework

namespace {

using json = nlohmann::json;
using namespace agent_framework;

ToolMeta meta_x_int() {
    ToolMeta m;
    m.name = "t";
    m.description = "x int";
    m.schema = json::parse(
        R"({"type":"object","properties":{"x":{"type":"integer"}},"required":["x"]})");
    return m;
}

void test_h1_no_hook_behaves_like_before() {
    ToolBus bus;
    ToolMeta m = meta_x_int();
    m.name = "t";
    bus.register_local_tool(
        "t", [](const json& j) { return json{{"v", j.at("x").get<int>()}}; }, m);
    assert(bus.tool_call_hook_count() == 0U);
    json r = bus.call_tool("t", json{{"x", 7}}).get();
    assert(r.at("v").get<int>() == 7);
}

void test_h2_deny_skips_tool() {
    ToolBus bus;
    ToolMeta m = meta_x_int();
    m.name = "t";
    std::atomic<int> calls{0};
    bus.register_local_tool(
        "t",
        [&calls](const json& j) {
            ++calls;
            return j;
        },
        m);
    bus.add_tool_call_hook([](const std::string&, const json&) {
        ToolHookResult out;
        out.verdict = ToolHookVerdict::Deny;
        out.deny_message = "blocked";
        out.deny_details = json{{"reason", "test"}};
        return out;
    });
    json r = bus.call_tool("t", json{{"x", 1}}).get();
    assert(r.at("code") == "hook_denied");
    assert(r.at("error") == "blocked");
    assert(r.at("details")["hook_index"] == 0);
    assert(r.at("details")["reason"] == "test");
    assert(calls.load() == 0);
}

void test_h3_replace_then_validate_and_call() {
    ToolBus bus;
    ToolMeta m = meta_x_int();
    m.name = "t";
    json seen = json::object();
    bus.register_local_tool(
        "t",
        [&seen](const json& j) {
            seen = j;
            return json{{"ok", true}};
        },
        m);
    bus.add_tool_call_hook([](const std::string&, const json&) {
        ToolHookResult out;
        out.verdict = ToolHookVerdict::Replace;
        out.replaced_arguments = json{{"x", 99}};
        return out;
    });
    json r = bus.call_tool("t", json::object()).get();
    assert(r.at("ok") == true);
    assert(seen.at("x").get<int>() == 99);
}

void test_h4_two_hooks_second_sees_replaced() {
    ToolBus bus;
    ToolMeta m = meta_x_int();
    m.name = "t";
    bus.register_local_tool("t", [](const json& j) { return json{{"x", j.at("x")}}; }, m);
    bus.add_tool_call_hook([](const std::string&, const json&) {
        ToolHookResult out;
        out.verdict = ToolHookVerdict::Replace;
        out.replaced_arguments = json{{"x", 42}};
        return out;
    });
    bus.add_tool_call_hook([](const std::string&, const json& args) {
        assert(args.at("x").get<int>() == 42);
        ToolHookResult out;
        out.verdict = ToolHookVerdict::Allow;
        return out;
    });
    json r = bus.call_tool("t", json{{"x", 1}}).get();
    assert(r.at("x").get<int>() == 42);
}

void test_h5_allowlist_blocks_before_hooks() {
    ToolBus bus;
    std::atomic<int> hook_calls{0};
    bus.add_tool_call_hook([&hook_calls](const std::string&, const json&) {
        ++hook_calls;
        ToolHookResult out;
        out.verdict = ToolHookVerdict::Allow;
        return out;
    });

    ToolMeta m = meta_x_int();
    m.name = "ghost";
    detail::ToolBusCallHookTestPeer::emplace_tool(
        bus, "ghost", [](const json& j) { return json{{"got", j}}; }, m);

    json r = bus.call_tool("ghost", json{{"x", 1}}).get();
    assert(r.at("code") == "tool_not_allowed");
    assert(hook_calls.load() == 0);
}

void test_h6_hook_throws() {
    ToolBus bus;
    ToolMeta m = meta_x_int();
    m.name = "t";
    std::atomic<int> calls{0};
    bus.register_local_tool(
        "t",
        [&calls](const json& j) {
            ++calls;
            return j;
        },
        m);
    bus.add_tool_call_hook([](const std::string&, const json&) -> ToolHookResult {
        throw std::runtime_error("boom");
    });
    json r = bus.call_tool("t", json{{"x", 1}}).get();
    assert(r.at("code") == "hook_threw");
    assert(r.at("details")["exception"] == "boom");
    assert(calls.load() == 0);
}

void test_h7_invalid_replace() {
    ToolBus bus;
    ToolMeta m = meta_x_int();
    m.name = "t";
    bus.register_local_tool("t", [](const json& j) { return j; }, m);
    bus.add_tool_call_hook([](const std::string&, const json&) {
        ToolHookResult out;
        out.verdict = ToolHookVerdict::Replace;
        return out;
    });
    json r = bus.call_tool("t", json{{"x", 1}}).get();
    assert(r.at("code") == "hook_invalid_replace");
}

void test_replace_then_validation_fails_no_tool_call() {
    ToolBus bus;
    ToolMeta m = meta_x_int();
    m.name = "t";
    std::atomic<int> calls{0};
    bus.register_local_tool(
        "t",
        [&calls](const json& j) {
            ++calls;
            return j;
        },
        m);
    bus.add_tool_call_hook([](const std::string&, const json&) {
        ToolHookResult out;
        out.verdict = ToolHookVerdict::Replace;
        out.replaced_arguments = json{{"y", 1}};
        return out;
    });
    json r = bus.call_tool("t", json{{"x", 1}}).get();
    assert(r.at("code") == "validation_failed");
    assert(calls.load() == 0);
}

int run_all_but_h5() {
    test_h1_no_hook_behaves_like_before();
    test_h2_deny_skips_tool();
    test_h3_replace_then_validate_and_call();
    test_h4_two_hooks_second_sees_replaced();
    test_h6_hook_throws();
    test_h7_invalid_replace();
    test_replace_then_validation_fails_no_tool_call();
    std::cout << "test_tool_call_hooks: all passed\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--h5") == 0) {
#if defined(_WIN32)
        (void)_putenv_s("AGENT_TOOL_ALLOWLIST", "ok_only");
#else
        (void)::setenv("AGENT_TOOL_ALLOWLIST", "ok_only", 1);
#endif
        test_h5_allowlist_blocks_before_hooks();
        std::cout << "test_tool_call_hooks --h5: passed\n";
        return 0;
    }

#if defined(_WIN32)
    (void)_putenv_s("AGENT_TOOL_ALLOWLIST", "");
#else
    (void)::setenv("AGENT_TOOL_ALLOWLIST", "", 1);
#endif
    return run_all_but_h5();
}
