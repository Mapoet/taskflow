/**
 * @file test_a2a_jsonrpc.cpp
 * @brief WP2.1a JSON-RPC 通用层单测 JR-1–JR-7
 */

#include <agent/a2a/jsonrpc.hpp>

#include <cassert>
#include <iostream>
#include <string>
#include <variant>

using agent_framework::a2a::JsonRpcRequest;
using agent_framework::a2a::make_error_response;
using agent_framework::a2a::make_parse_error_response;
using agent_framework::a2a::make_success_response;
using agent_framework::a2a::parse_jsonrpc_request;
using agent_framework::a2a::try_get_jsonrpc_error_code;
using agent_framework::a2a::try_get_jsonrpc_response_id;
using agent_framework::a2a::try_get_jsonrpc_result;
namespace jc = agent_framework::a2a::JsonRpcErrorCode;

static void jr1_numeric_id() {
    std::string body = R"({"jsonrpc":"2.0","method":"m","params":{},"id":42})";
    auto pr = parse_jsonrpc_request(body);
    auto* req = std::get_if<JsonRpcRequest>(&pr);
    assert(req != nullptr);
    json resp = make_success_response(req->id, json{{"ok", true}});
    assert(try_get_jsonrpc_result(resp)->contains("ok"));
    json rid = try_get_jsonrpc_response_id(resp);
    assert(rid.is_number_integer() && rid.get<int>() == 42);
}

static void jr2_string_id() {
    std::string body = R"({"jsonrpc":"2.0","method":"m","params":{},"id":"req-1"})";
    auto pr = parse_jsonrpc_request(body);
    auto* req = std::get_if<JsonRpcRequest>(&pr);
    assert(req != nullptr);
    json resp = make_success_response(req->id, json{{"x", 1}});
    json rid = try_get_jsonrpc_response_id(resp);
    assert(rid.is_string() && rid.get<std::string>() == "req-1");
}

static void jr3_invalid_json() {
    auto pr = parse_jsonrpc_request("not json {");
    auto* err = std::get_if<json>(&pr);
    assert(err != nullptr);
    assert(try_get_jsonrpc_error_code(*err) == jc::parse_error);
    json rid = try_get_jsonrpc_response_id(*err);
    assert(rid.is_null());
}

static void jr4_missing_method() {
    std::string body = R"({"jsonrpc":"2.0","params":{},"id":1})";
    auto pr = parse_jsonrpc_request(body);
    auto* err = std::get_if<json>(&pr);
    assert(err != nullptr);
    assert(try_get_jsonrpc_error_code(*err) == jc::invalid_request);
}

static void jr5_method_not_found_shape() {
    std::string body = R"({"jsonrpc":"2.0","method":"unknown.method","params":{},"id":99})";
    auto pr = parse_jsonrpc_request(body);
    auto* req = std::get_if<JsonRpcRequest>(&pr);
    assert(req != nullptr);
    json resp = make_error_response(req->id, jc::method_not_found, "Method not found");
    assert(try_get_jsonrpc_error_code(resp) == jc::method_not_found);
    json rid = try_get_jsonrpc_response_id(resp);
    assert(rid.is_number_integer() && rid.get<int>() == 99);
}

static void jr6_invalid_params_shape() {
    std::string body = R"({"jsonrpc":"2.0","method":"m","params":{},"id":7})";
    auto pr = parse_jsonrpc_request(body);
    auto* req = std::get_if<JsonRpcRequest>(&pr);
    assert(req != nullptr);
    json resp = make_error_response(req->id, jc::invalid_params, "Invalid params");
    assert(try_get_jsonrpc_error_code(resp) == jc::invalid_params);
}

static void jr7_batch_array_root() {
    std::string body = R"([{"jsonrpc":"2.0","method":"m","id":1}])";
    auto pr = parse_jsonrpc_request(body);
    auto* err = std::get_if<json>(&pr);
    assert(err != nullptr);
    assert(try_get_jsonrpc_error_code(*err) == jc::invalid_request);
    assert(try_get_jsonrpc_response_id(*err).is_null());
}

static void extra_params_array_rejected() {
    std::string body = R"({"jsonrpc":"2.0","method":"m","params":[],"id":1})";
    auto pr = parse_jsonrpc_request(body);
    auto* err = std::get_if<json>(&pr);
    assert(err != nullptr);
    assert(try_get_jsonrpc_error_code(*err) == jc::invalid_request);
}

static void extra_missing_id() {
    std::string body = R"({"jsonrpc":"2.0","method":"m","params":{}})";
    auto pr = parse_jsonrpc_request(body);
    auto* err = std::get_if<json>(&pr);
    assert(err != nullptr);
    assert(try_get_jsonrpc_error_code(*err) == jc::invalid_request);
}

int main() {
    jr1_numeric_id();
    jr2_string_id();
    jr3_invalid_json();
    jr4_missing_method();
    jr5_method_not_found_shape();
    jr6_invalid_params_shape();
    jr7_batch_array_root();
    extra_params_array_rejected();
    extra_missing_id();
    (void)make_parse_error_response();
    std::cout << "a2a jsonrpc JR-1..JR-7 ok\n";
    return 0;
}
