/**
 * @file jsonrpc.cpp
 * @brief JSON-RPC 2.0 通用实现（WP2.1a）
 */

#include <agent/a2a/jsonrpc.hpp>

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace agent_framework {
namespace a2a {
namespace {

json invalid_request_response(const json& id_or_null, std::string_view message) {
    return make_error_response(id_or_null.is_null() ? json() : id_or_null,
                               JsonRpcErrorCode::invalid_request,
                               message);
}

bool is_valid_request_id(const json& id) {
    if (id.is_number_integer()) {
        return true;
    }
    if (id.is_string()) {
        return true;
    }
    return false;
}

} // namespace

json make_parse_error_response(std::string_view message) {
    json err = json::object();
    err["code"] = JsonRpcErrorCode::parse_error;
    err["message"] = std::string(message);
    json res = json::object();
    res["jsonrpc"] = "2.0";
    res["id"] = nullptr;
    res["error"] = std::move(err);
    return res;
}

json make_success_response(const json& id, const json& result) {
    json res = json::object();
    res["jsonrpc"] = "2.0";
    res["id"] = id;
    res["result"] = result;
    return res;
}

json make_error_response(const json& id, int code, std::string_view message, const json& data) {
    json err = json::object();
    err["code"] = code;
    err["message"] = std::string(message);
    if (!data.is_null()) {
        err["data"] = data;
    }
    json res = json::object();
    res["jsonrpc"] = "2.0";
    res["id"] = id.is_null() ? json(nullptr) : id;
    res["error"] = std::move(err);
    return res;
}

JsonRpcParseResult parse_jsonrpc_request(std::string_view body_utf8) {
    json root;
    try {
        root = json::parse(body_utf8);
    } catch (const json::parse_error&) {
        return make_parse_error_response("Parse error");
    }

    if (root.is_array()) {
        return invalid_request_response(json(), "Invalid Request");
    }
    if (!root.is_object()) {
        return invalid_request_response(json(), "Invalid Request");
    }

    if (!root.contains("jsonrpc") || !root["jsonrpc"].is_string() ||
        root["jsonrpc"].get<std::string>() != "2.0") {
        json id_field = root.contains("id") ? root["id"] : json();
        if (id_field.is_null() || (!id_field.is_number() && !id_field.is_string())) {
            id_field = json();
        }
        return invalid_request_response(id_field.is_null() ? json() : id_field, "Invalid Request");
    }

    if (!root.contains("method") || !root["method"].is_string()) {
        json id_guess = root.contains("id") ? root["id"] : json();
        if (!is_valid_request_id(id_guess)) {
            id_guess = json();
        }
        return invalid_request_response(id_guess, "Invalid Request");
    }

    if (root.contains("params")) {
        const json& p = root["params"];
        if (!p.is_object()) {
            json id_guess = root.contains("id") ? root["id"] : json();
            if (!is_valid_request_id(id_guess)) {
                id_guess = json();
            }
            return invalid_request_response(id_guess, "Invalid Request");
        }
    }

    if (!root.contains("id")) {
        return invalid_request_response(json(), "Invalid Request");
    }

    const json& id = root["id"];
    if (id.is_null()) {
        return invalid_request_response(json(), "Invalid Request");
    }
    if (!is_valid_request_id(id)) {
        return invalid_request_response(json(), "Invalid Request");
    }

    JsonRpcRequest req;
    req.method = root["method"].get<std::string>();
    if (root.contains("params")) {
        req.params = root["params"];
    }
    req.id = id;
    return req;
}

std::optional<int> try_get_jsonrpc_error_code(const json& response) {
    if (!response.is_object() || !response.contains("error")) {
        return std::nullopt;
    }
    const auto& e = response["error"];
    if (!e.is_object() || !e.contains("code")) {
        return std::nullopt;
    }
    const auto& c = e["code"];
    if (c.is_number_integer()) {
        return c.get<int>();
    }
    if (c.is_number_unsigned()) {
        return static_cast<int>(c.get<std::uint64_t>());
    }
    if (c.is_number_float()) {
        return static_cast<int>(c.get<double>());
    }
    if (c.is_string()) {
        const std::string s = c.get<std::string>();
        char* end = nullptr;
        const long v = std::strtol(s.c_str(), &end, 10);
        if (end != s.c_str() && *end == '\0' && v >= INT_MIN && v <= INT_MAX) {
            return static_cast<int>(v);
        }
    }
    return std::nullopt;
}

std::optional<json> try_get_jsonrpc_result(const json& response) {
    if (!response.is_object() || !response.contains("result")) {
        return std::nullopt;
    }
    return response["result"];
}

json try_get_jsonrpc_response_id(const json& response) {
    if (!response.is_object() || !response.contains("id")) {
        return json();
    }
    return response["id"];
}

} // namespace a2a
} // namespace agent_framework
