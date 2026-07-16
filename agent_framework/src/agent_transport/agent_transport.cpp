/**
 * @file agent_transport.cpp
 * @brief Agent 传输层实现（HTTP + JSON-RPC 2.0，底层使用 HttplibClient）
 */
#include <agent/agent_transport/agent_transport.hpp>
#include <agent/a2a/jsonrpc_client.hpp>
#include <agent/agent_client/httplib_http_client.hpp>

#include <map>
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

    auto* hc = dynamic_cast<HttplibClient*>(http_client_.get());
    if (!hc) {
        throw std::runtime_error("HTTPAgentTransport: expected HttplibClient");
    }

    const std::string url = AgentClient::join_url(base_url_, current_endpoint_);
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    return a2a::a2a_jsonrpc_post(*hc, url, method, params, headers, jsonrpc_next_id_);
}

bool HTTPAgentTransport::is_connected() const {
    return connected_;
}

std::string HTTPAgentTransport::get_transport_type() const {
    return "http";
}

} // namespace agent_framework
