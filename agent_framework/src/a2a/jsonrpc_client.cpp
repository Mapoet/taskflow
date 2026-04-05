/**
 * @file jsonrpc_client.cpp
 * @brief JSON-RPC POST client helper (WP2.4)
 */
#include <agent/a2a/jsonrpc_client.hpp>
#include <agent/httplib_http_client.hpp>

namespace agent_framework {
namespace a2a {

A2aRpcException::A2aRpcException(int code, std::string message, json data)
    : std::runtime_error("A2aRpcException: " + message + " (code=" + std::to_string(code) + ")"),
      code_(code),
      data_(std::move(data)) {}

json a2a_jsonrpc_post(
    HttplibClient& client,
    const std::string& full_rpc_url,
    std::string_view method,
    const json& params,
    const std::map<std::string, std::string>& headers,
    std::atomic<std::uint64_t>& next_id) {

    const std::uint64_t request_id = next_id.fetch_add(1, std::memory_order_relaxed);

    json request = {
        {"jsonrpc", "2.0"},
        {"method", std::string(method)},
        {"params", params},
        {"id", request_id}
    };

    std::map<std::string, std::string> h = headers;
    if (h.find("Content-Type") == h.end()) {
        h["Content-Type"] = "application/json";
    }

    json response = client.post(full_rpc_url, request, h);

    if (auto ec = try_get_jsonrpc_error_code(response)) {
        std::string msg = "JSON-RPC error";
        json data = json::object();
        if (response.contains("error") && response["error"].is_object()) {
            const json& e = response["error"];
            if (e.contains("message") && e["message"].is_string()) {
                msg = e["message"].get<std::string>();
            }
            if (e.contains("data")) {
                data = e["data"];
            }
        }
        throw A2aRpcException(*ec, std::move(msg), std::move(data));
    }

    if (auto r = try_get_jsonrpc_result(response)) {
        return *r;
    }

    throw A2aRpcException(JsonRpcErrorCode::internal_error, "JSON-RPC response missing result",
                          json::object());
}

} // namespace a2a
} // namespace agent_framework
