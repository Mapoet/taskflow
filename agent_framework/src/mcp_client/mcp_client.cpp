/**
 * @file mcp_client.cpp
 * @brief MCPClient、WebSocket 占位
 */

#include "agent/mcp_client.hpp"
#include "agent/mcp_protocol.hpp"

#include <future>
#include <stdexcept>
#include <string>
#include <utility>

namespace agent_framework {

json MCPClient::parse_jsonrpc_response(const json& response, std::int64_t expected_id) {
    if (!response.is_object()) {
        throw std::runtime_error("MCP: response is not a JSON object");
    }
    if (response.contains("error") && !response["error"].is_null()) {
        const auto& err = response["error"];
        std::string msg = err.dump();
        if (err.is_object() && err.contains("message") && err["message"].is_string()) {
            msg = err["message"].get<std::string>();
        }
        throw std::runtime_error("MCP JSON-RPC error: " + msg);
    }
    if (!response.contains("id")) {
        throw std::runtime_error("MCP: missing id in response");
    }
    std::int64_t rid = 0;
    const auto& jid = response["id"];
    if (jid.is_number_integer()) {
        rid = jid.get<std::int64_t>();
    } else if (jid.is_number_unsigned()) {
        rid = static_cast<std::int64_t>(jid.get<std::uint64_t>());
    } else {
        throw std::runtime_error("MCP: response id is not an integer");
    }
    if (rid != expected_id) {
        throw std::runtime_error("MCP: id mismatch in response");
    }
    if (!response.contains("result")) {
        return json::object();
    }
    return response["result"];
}

MCPClient::MCPClient(std::unique_ptr<MCPTransportInterface> transport)
    : transport_(std::move(transport)) {}

MCPClient::~MCPClient() {
    if (transport_) {
        transport_->disconnect();
    }
}

void MCPClient::handshake() {
    json params = {{"protocolVersion", std::string(mcp_protocol::k_protocol_version)},
                   {"capabilities", json::object()},
                   {"clientInfo",
                    json{{"name", "agent_framework"}, {"version", "1.0.0"}}}};
    (void)send_jsonrpc_request(std::string(mcp_protocol::k_method_initialize), params);
    json note = {{"jsonrpc", std::string(mcp_protocol::k_jsonrpc_version)},
                 {"method", std::string(mcp_protocol::k_method_notifications_initialized)},
                 {"params", json::object()}};
    transport_->send_notification(note);
}

std::shared_ptr<MCPClient> MCPClient::create_stdio(const std::string& command,
                                                   const std::vector<std::string>& args) {
    auto tr = std::make_unique<StdioMCPTransport>(command, args);
    if (!tr->connect("")) {
        throw std::runtime_error("MCP initialize failed: stdio connect()");
    }
    std::shared_ptr<MCPClient> c(new MCPClient(std::move(tr)));
    try {
        c->handshake();
    } catch (...) {
        c->disconnect();
        throw;
    }
    return c;
}

std::shared_ptr<MCPClient> MCPClient::create_stdio(const std::string& command,
                                                   const std::vector<std::string>& args,
                                                   const std::map<std::string, std::string>& extra_env) {
    auto tr = std::make_unique<StdioMCPTransport>(command, args, extra_env);
    if (!tr->connect("")) {
        throw std::runtime_error("MCP initialize failed: stdio connect()");
    }
    std::shared_ptr<MCPClient> c(new MCPClient(std::move(tr)));
    try {
        c->handshake();
    } catch (...) {
        c->disconnect();
        throw;
    }
    return c;
}

std::shared_ptr<MCPClient> MCPClient::create_http(const std::string& post_url,
                                                  const std::map<std::string, std::string>& extra_headers) {
    auto tr = std::make_unique<HttpMCPTransport>(post_url, extra_headers);
    if (!tr->connect("")) {
        throw std::runtime_error("MCP initialize failed: http connect()");
    }
    std::shared_ptr<MCPClient> c(new MCPClient(std::move(tr)));
    try {
        c->handshake();
    } catch (...) {
        c->disconnect();
        throw;
    }
    return c;
}

std::shared_ptr<MCPClient> MCPClient::create_with_transport(std::unique_ptr<MCPTransportInterface> transport,
                                                            bool run_handshake) {
    if (!transport) {
        throw std::invalid_argument("MCPClient::create_with_transport: null transport");
    }
    if (!transport->connect("")) {
        throw std::runtime_error("MCPClient::create_with_transport: connect failed");
    }
    std::shared_ptr<MCPClient> c(new MCPClient(std::move(transport)));
    if (run_handshake) {
        try {
            c->handshake();
        } catch (...) {
            c->disconnect();
            throw;
        }
    }
    return c;
}

json MCPClient::send_jsonrpc_request(const std::string& method, const json& params,
                                     const std::function<bool()>& cancellation_requested) {
    std::lock_guard<std::mutex> lock(rpc_mutex_);
    std::int64_t id = next_id_.fetch_add(1);
    json req = {{"jsonrpc", std::string(mcp_protocol::k_jsonrpc_version)},
                {"id", id},
                {"method", method},
                {"params", params}};
    json resp = transport_->transceive_cancellable(req, cancellation_requested);
    return parse_jsonrpc_response(resp, id);
}

static json unwrap_tool_result(const json& result) {
    if (result.is_object() && result.contains("isError") && result["isError"].is_boolean() &&
        result["isError"].get<bool>()) {
        return json{{"error", "MCP tool execution reported isError"},
                    {"code", "mcp_tool_error"},
                    {"details", result}};
    }
    return result;
}

std::future<std::vector<ToolMeta>> MCPClient::list_tools() {
    return std::async(std::launch::async, [this]() {
        json result = send_jsonrpc_request(std::string(mcp_protocol::k_method_tools_list), json::object());
        std::vector<ToolMeta> out;
        if (!result.contains("tools") || !result["tools"].is_array()) {
            return out;
        }
        for (const auto& t : result["tools"]) {
            ToolMeta m;
            m.name = t.at("name").get<std::string>();
            if (t.contains("description") && t["description"].is_string()) {
                m.description = t["description"].get<std::string>();
            }
            if (t.contains("inputSchema")) {
                m.schema = t["inputSchema"];
            } else {
                m.schema = json{{"type", "object"}, {"properties", json::object()}};
            }
            out.push_back(std::move(m));
        }
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cached_tools_ = out;
        }
        return out;
    });
}

std::future<json> MCPClient::call_tool(const std::string& name, const json& arguments,
                                       std::function<bool()> cancellation_requested) {
    return std::async(std::launch::async, [this, name, arguments,
                                           cancellation_requested = std::move(cancellation_requested)]() {
        try {
            json params = json{{"name", name}, {"arguments", arguments}};
            json result =
                send_jsonrpc_request(std::string(mcp_protocol::k_method_tools_call), params,
                                     cancellation_requested);
            return unwrap_tool_result(result);
        } catch (const std::exception& e) {
            return json{{"error", e.what()}, {"code", "mcp_jsonrpc_error"}, {"details", json::object()}};
        } catch (...) {
            return json{{"error", "unknown exception in MCP call_tool"},
                        {"code", "mcp_jsonrpc_error"},
                        {"details", json::object()}};
        }
    });
}

bool MCPClient::ping() {
    return transport_ && transport_->is_connected();
}

void MCPClient::disconnect() {
    if (transport_) {
        transport_->disconnect();
    }
}

bool MCPClient::is_connected() const {
    return transport_ && transport_->is_connected();
}

// --- WebSocket (WP1.3 占位) ---

WebSocketMCPTransport::WebSocketMCPTransport(const std::string& ws_url) : ws_url_(ws_url) {}

WebSocketMCPTransport::~WebSocketMCPTransport() {
    disconnect();
}

bool WebSocketMCPTransport::connect(const std::string& /*endpoint*/) {
    connected_ = false;
    return false;
}

void WebSocketMCPTransport::disconnect() {
    std::lock_guard<std::mutex> lock(ws_mutex_);
    connection_ = nullptr;
    connected_ = false;
}

json WebSocketMCPTransport::transceive(const json& /*jsonrpc_request*/) {
    return json{{"error", "WebSocket MCP transport not implemented"},
                {"code", "mcp_transport_unsupported"},
                {"details", json{{"transport", "websocket"}}}};
}

void WebSocketMCPTransport::send_notification(const json& /*jsonrpc_notification*/) {}

bool WebSocketMCPTransport::is_connected() const {
    return connected_;
}

MCPTransport WebSocketMCPTransport::get_transport_type() const {
    return MCPTransport::WEBSOCKET;
}

void WebSocketMCPTransport::handle_message(void* /*hdl*/, void* /*msg*/) {}

} // namespace agent_framework
