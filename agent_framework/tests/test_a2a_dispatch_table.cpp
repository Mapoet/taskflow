/**
 * @file test_a2a_dispatch_table.cpp
 * @brief JR-5 / JR-6 风格：未知 method、非法 SendMessage params
 */
#include <agent/a2a/dispatch_table.hpp>
#include <agent/a2a/jsonrpc.hpp>
#include <agent/a2a/wire_mapping.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

namespace af = agent_framework;
namespace a2a = af::a2a;

static void jr5_unknown_method() {
    a2a::DispatchTable table;
    try {
        table.invoke("NonExistent", json::object());
        throw std::runtime_error("jr5 expected throw");
    } catch (const a2a::JsonRpcInvokeError& e) {
        if (e.code != -32601) {
            throw std::runtime_error("jr5 code");
        }
    }
}

static void jr6_invalid_send_message_params() {
    a2a::DispatchTable table;
    table.register_method("SendMessage", [](const json& params) -> json {
        a2a::validate_send_message_params_for_dispatch(params);
        af::AgentTask t;
        t.task_id = "stub";
        t.status = af::AgentTaskStatus::PENDING;
        t.updated_at = std::chrono::system_clock::now();
        t.metadata = json::object();
        return json{{"task", a2a::task_to_a2a_wire(t)}};
    });
    try {
        table.invoke("SendMessage", json::object());
        throw std::runtime_error("jr6 expected throw");
    } catch (const a2a::JsonRpcInvokeError& e) {
        if (e.code != -32602) {
            throw std::runtime_error("jr6 code");
        }
    }
    json good_params;
    good_params["message"] = json{{"role", "ROLE_USER"}, {"parts", json::array()}};
    json result = table.invoke("SendMessage", good_params);
    if (!result.contains("task")) {
        throw std::runtime_error("jr6 result");
    }
}

static void parse_then_invoke_round_trip() {
    std::string body = R"({"jsonrpc":"2.0","method":"SendMessage","params":{"message":{"role":"ROLE_USER","parts":[]}},"id":1})";
    auto pr = a2a::parse_jsonrpc_request(body);
    auto* req = std::get_if<a2a::JsonRpcRequest>(&pr);
    if (!req) {
        throw std::runtime_error("parse");
    }
    a2a::DispatchTable table;
    table.register_method("SendMessage", [](const json& params) -> json {
        a2a::validate_send_message_params_for_dispatch(params);
        return json{{"task", json{{"id", "x"}, {"status", json{{"state", "TASK_STATE_SUBMITTED"}, {"timestamp", "2026-04-05T00:00:00.000Z"}}}, {"history", json::array()}, {"artifacts", json::array()}, {"metadata", json::object()}}}};
    });
    json res = table.invoke(req->method, req->params.value_or(json::object()));
    json envelope = a2a::make_success_response(req->id, res);
    if (!envelope.contains("result")) {
        throw std::runtime_error("envelope");
    }
}

int main() {
    jr5_unknown_method();
    jr6_invalid_send_message_params();
    parse_then_invoke_round_trip();
    std::cout << "test_a2a_dispatch_table: ok\n";
    return 0;
}
