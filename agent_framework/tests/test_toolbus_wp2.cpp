/**
 * @file test_toolbus_wp2.cpp
 * @brief WP1.2 ToolBus / schema_validate 单测
 *
 * 运行：`test_toolbus_wp2`（默认套件，勿设置 AGENT_TOOL_ALLOWLIST）
 *      `test_toolbus_wp2 --allowlist`（配合 ctest 设置 AGENT_TOOL_ALLOWLIST=add）
 */

#include <agent/schema_validate.hpp>
#include <agent/toolbus.hpp>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using json = nlohmann::json;
using agent_framework::ToolBus;
using agent_framework::ToolMeta;
using agent_framework::validate_tool_arguments;

ToolMeta make_add_meta() {
    ToolMeta m;
    m.name = "add";
    m.description = "sum two integers";
    m.schema = json::parse(R"({
        "type": "object",
        "properties": {
            "a": { "type": "integer" },
            "b": { "type": "integer" }
        },
        "required": ["a", "b"]
    })");
    return m;
}

void test_schema_required() {
    json schema = json::parse(R"({"type":"object","properties":{"x":{"type":"string"}},"required":["x"]})");
    json err;
    assert(!validate_tool_arguments(schema, json::object(), err));
    assert(err["code"] == "validation_failed");
    assert(err["details"].contains("missing"));
}

void test_schema_type() {
    json schema =
        json::parse(R"({"type":"object","properties":{"n":{"type":"integer"}},"required":["n"]})");
    json err;
    assert(!validate_tool_arguments(schema, json{{"n", "oops"},}, err));
    assert(err["code"] == "validation_failed");
}

void test_schema_additional_props_implicit_false() {
    json schema = json::parse(R"({"type":"object","properties":{},"required":[]})");
    json err;
    assert(!validate_tool_arguments(schema, json{{"extra", 1}}, err));
    assert(err["code"] == "validation_failed");
}

void test_schema_enum() {
    json schema = json::parse(
        R"({"type":"object","properties":{"c":{"type":"string","enum":["a","b"]}},"required":["c"]})");
    json err;
    assert(!validate_tool_arguments(schema, json{{"c", "z"}}, err));
    assert(err["code"] == "validation_failed");
}

void test_schema_ref_rejected() {
    json schema = json::parse(R"({"type":"object","$ref":"#/defs/x"})");
    json err;
    assert(!validate_tool_arguments(schema, json::object(), err));
    assert(err["code"] == "schema_unsupported");
}

void test_add_success() {
    ToolBus bus;
    ToolMeta meta = make_add_meta();
    bus.register_local_tool(
        "add",
        [](const json& j) {
            return json{{"result", j.at("a").get<int>() + j.at("b").get<int>()}};
        },
        meta);
    auto fut = bus.call_tool("add", json{{"a", 2}, {"b", 3}});
    json r = fut.get();
    assert(r.at("result").get<int>() == 5);
}

void test_unknown_tool() {
    ToolBus bus;
    auto fut = bus.call_tool("nope", json::object());
    json r = fut.get();
    assert(r.at("code") == "unknown_tool");
}

void test_validation_failed_call() {
    ToolBus bus;
    bus.register_local_tool(
        "add",
        [](const json& j) { return json{{"result", j.at("a").get<int>() + j.at("b").get<int>()}}; },
        make_add_meta());
    auto fut = bus.call_tool("add", json{{"a", 1}}); // missing b
    json r = fut.get();
    assert(r.at("code") == "validation_failed");
}

void test_duplicate_register_throws() {
    ToolBus bus;
    ToolMeta m = make_add_meta();
    bus.register_local_tool("add", [](const json& j) { return j; }, m);
    try {
        bus.register_local_tool("add", [](const json& j) { return j; }, m);
        assert(false);
    } catch (const std::invalid_argument&) {
    }
}

void test_register_mcp_throws() {
    ToolBus bus;
    try {
        bus.register_mcp_service("svc", nullptr);
        assert(false);
    } catch (const std::invalid_argument&) {
    }
    try {
        bus.register_api_tool("x", "http://x", "POST", make_add_meta());
        assert(false);
    } catch (const std::logic_error& e) {
        assert(std::strstr(e.what(), "WP1.3") != nullptr);
    }
}

void test_export_order() {
    ToolBus bus;
    ToolMeta mz;
    mz.name = "zed";
    mz.schema = json{{"type", "object"}, {"properties", json::object()}};
    ToolMeta ma;
    ma.name = "alpha";
    ma.schema = mz.schema;
    bus.register_local_tool("zed", [](const json& j) { return j; }, mz);
    bus.register_local_tool("alpha", [](const json& j) { return j; }, ma);
    auto tools = bus.export_as_llm_tools();
    assert(tools.size() == 2U);
    assert(tools[0].name == "alpha");
    assert(tools[1].name == "zed");
    auto names = bus.list_all_tools();
    assert(names.size() == 2U);
    assert(names[0] == "alpha");
    assert(names[1] == "zed");
}

void test_local_tool_name_mismatch_future() {
    agent_framework::LocalTool t(
        "ok", [](const json& j) { return j; },
        make_add_meta());
    auto fut = t.call("wrong", json::object());
    json r = fut.get();
    assert(r.at("code") == "tool_internal_error");
}

void test_get_tool_info() {
    ToolBus bus;
    bus.register_local_tool("add", [](const json& j) { return j; }, make_add_meta());
    auto info = bus.get_tool_info("add");
    assert(info.has_value());
    assert(info->name == "add");
    assert(!bus.get_tool_info("missing").has_value());
}

int run_core_tests() {
    test_schema_required();
    test_schema_type();
    test_schema_additional_props_implicit_false();
    test_schema_enum();
    test_schema_ref_rejected();
    test_add_success();
    test_unknown_tool();
    test_validation_failed_call();
    test_duplicate_register_throws();
    test_register_mcp_throws();
    test_export_order();
    test_local_tool_name_mismatch_future();
    test_get_tool_info();
    std::cout << "test_toolbus_wp2: all core tests passed\n";
    return 0;
}

int run_allowlist_tests() {
    const char* al = std::getenv("AGENT_TOOL_ALLOWLIST");
    if (al == nullptr || std::string(al).find("add") == std::string::npos) {
        std::cerr << "run_allowlist_tests: need AGENT_TOOL_ALLOWLIST containing add\n";
        return 2;
    }
    ToolBus bus;
    ToolMeta meta = make_add_meta();
    bus.register_local_tool(
        "add",
        [](const json& j) {
            return json{{"result", j.at("a").get<int>() + j.at("b").get<int>()}};
        },
        meta);
    try {
        ToolMeta other = meta;
        other.name = "other";
        bus.register_local_tool("other", [](const json& j) { return j; }, other);
        assert(false);
    } catch (const std::invalid_argument&) {
    }
    auto r = bus.call_tool("add", json{{"a", 1}, {"b", 1}}).get();
    assert(r.at("result").get<int>() == 2);
    std::cout << "test_toolbus_wp2: allowlist tests passed\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--allowlist") == 0) {
        return run_allowlist_tests();
    }
    return run_core_tests();
}
