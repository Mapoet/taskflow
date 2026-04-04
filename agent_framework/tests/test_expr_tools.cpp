/**
 * @file test_expr_tools.cpp
 * @brief 内建 expr_eval / expr_validate / expr_batch_eval（ExprTk）；门闩、幂等、allowlist 分进程用例。
 */
#include <agent/expr_tools.hpp>
#include <agent/toolbus.hpp>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using json = nlohmann::json;

void allowlist_partial_mode() {
    agent_framework::ToolBus bus;
    (void)::setenv("AGENT_EXPR_ENABLE", "1", 1);
    (void)::setenv("AGENT_TOOL_ALLOWLIST", "expr_eval", 1);
    try {
        agent_framework::register_builtin_expr_tools_if_configured(bus);
        std::cerr << "test_expr_tools: expected invalid_argument from partial allowlist\n";
        std::exit(1);
    } catch (const std::invalid_argument& e) {
        const std::string m = e.what();
        if (m.find("AGENT_TOOL_ALLOWLIST") == std::string::npos) {
            std::cerr << "test_expr_tools: unexpected message: " << m << '\n';
            std::exit(1);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "--allowlist-partial") {
        allowlist_partial_mode();
        return 0;
    }

    using namespace agent_framework;

    (void)::setenv("AGENT_TOOL_ALLOWLIST", "", 1);
    (void)::unsetenv("AGENT_EXPR_ENABLE");

    {
        (void)::setenv("AGENT_EXPR_ENABLE", "0", 1);
        ToolBus bus;
        register_builtin_expr_tools_if_configured(bus);
        assert(!bus.get_tool_info("expr_eval").has_value());
        (void)::unsetenv("AGENT_EXPR_ENABLE");
    }

    {
        ToolBus bus;
        register_builtin_expr_tools_if_configured(bus);
        register_builtin_expr_tools_if_configured(bus);
        assert(bus.get_tool_info("expr_eval").has_value());
        assert(bus.get_tool_info("expr_validate").has_value());
        assert(bus.get_tool_info("expr_batch_eval").has_value());

        json r = bus.call_tool("expr_eval", json{{"expression", "2*3+1"}}).get();
        assert(!r.contains("error"));
        assert(r["value"].get<double>() == 7.0);

        r = bus.call_tool("expr_eval",
                          json{{"expression", "x*y+1"},
                               {"variables", json{{"x", 2.0}, {"y", 3.0}}}})
                .get();
        assert(!r.contains("error"));
        assert(r["value"].get<double>() == 7.0);

        r = bus.call_tool("expr_eval",
                          json{{"expression", "x := 2"},
                               {"variables", json{{"x", 1.0}}}})
                .get();
        assert(r.contains("error"));
        assert(r["error"]["code"] == "parse_error" || r["error"]["code"] == "undefined_symbol");

        r = bus.call_tool(
                 "expr_validate",
                 json{{"expression", "x + 1"}, {"variables", json{{"x", 0.0}}}})
                .get();
        assert(!r.contains("error"));
        assert(r["ok"].get<bool>() == true);
        assert(r["variables"].is_array());
        bool saw_x = false;
        for (const auto& item : r["variables"]) {
            if (item["name"].get<std::string>() == "x") {
                saw_x = true;
                break;
            }
        }
        assert(saw_x);

        r = bus
                 .call_tool("expr_batch_eval",
                            json{{"expression", "a + b"},
                                 {"rows",
                                  json::array({json{{"variables", json{{"a", 1.0}, {"b", 2.0}}}},
                                               json{{"variables", json{{"a", 10.0}, {"b", 0.5}}}}})}})
                 .get();
        assert(!r.contains("error"));
        assert(r["values"].is_array());
        assert(r["values"].size() == 2U);
        assert(r["values"][0].get<double>() == 3.0);
        assert(r["values"][1].get<double>() == 10.5);
    }

    {
        (void)::setenv("AGENT_EXPR_MAX_LOOP_ITERS", "30", 1);
        ToolBus bus_loop;
        register_builtin_expr_tools_if_configured(bus_loop);
        json r = bus_loop.call_tool(
                         "expr_eval",
                         json{{"expression", "for (var i := 0; i < 100; i += 1) { 1; }"}})
                         .get();
        (void)::unsetenv("AGENT_EXPR_MAX_LOOP_ITERS");
        assert(r.contains("error"));
        assert(r["error"]["code"] == "loop_limit");
    }

    return 0;
}
