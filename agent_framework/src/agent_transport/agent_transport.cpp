/**
 * @file agent_transport.cpp
 * @brief Agent 传输层实现（HTTP + JSON-RPC 2.0，底层使用 HttplibClient）
 */
#include <agent/agent_transport.hpp>
#include <agent/httplib_http_client.hpp>

#include <stdexcept>

namespace agent_framework {

HTTPAgentTransport::HTTPAgentTransport(const std::string& base_url)
    : base_url_(base_url), connected_(false), http_client_(std::make_unique<HttplibClient>()) {
}

HTTPAgentTransport::~HTTPAgentTransport() {
    disconnect();
}

bool HTTPAgentTransport::connect(const std::string& endpoint) {
    current_endpoint_ = endpoint;
    connected_ = true;
    return true;
}

void HTTPAgentTransport::disconnect() {
    connected_ = false;
    current_endpoint_.clear();
}

json HTTPAgentTransport::send_request(const std::string& method, const json& params) {
    if (!connected_) {
        throw std::runtime_error("HTTPAgentTransport: not connected");
    }

    const std::uint64_t request_id =
        jsonrpc_next_id_.fetch_add(1, std::memory_order_relaxed);

    json request = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params},
        {"id", request_id}
    };

    return send_http_post(request);
}

bool HTTPAgentTransport::is_connected() const {
    return connected_;
}

std::string HTTPAgentTransport::get_transport_type() const {
    return "http";
}

json HTTPAgentTransport::send_http_post(const json& payload) {
    const std::string url = AgentClient::join_url(base_url_, current_endpoint_);
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    json response = http_client_->post(url, payload, headers);

    if (response.contains("error")) {
        const auto& err = response["error"];
        if (err.is_object() && err.contains("message")) {
            throw std::runtime_error("JSON-RPC error: " + err["message"].get<std::string>());
        }
        throw std::runtime_error("JSON-RPC error: unknown error object");
    }

    return response;
}

} // namespace agent_framework
