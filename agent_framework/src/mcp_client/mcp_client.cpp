/**
 * @file mcp_client.cpp
 * @brief MCPClient、WebSocket 占位
 */

#include <agent/mcp_client/mcp_client.hpp>
#include <agent/mcp_client/mcp_protocol.hpp>

#include <future>
#include <cstdlib>
#include <limits>
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
    const json result =
        send_jsonrpc_request(std::string(mcp_protocol::k_method_initialize), params);
    resources_supported_ = result.contains("capabilities") &&
                           result["capabilities"].is_object() &&
                           result["capabilities"].contains("resources") &&
                           result["capabilities"]["resources"].is_object();
    tools_supported_ = result.contains("capabilities") &&
                       result["capabilities"].is_object() &&
                       result["capabilities"].contains("tools") &&
                       result["capabilities"]["tools"].is_object();
    json note = {{"jsonrpc", std::string(mcp_protocol::k_jsonrpc_version)},
                 {"method", std::string(mcp_protocol::k_method_notifications_initialized)},
                 {"params", json::object()}};
    transport_->send_notification(note);
}

std::shared_ptr<MCPClient> MCPClient::create_stdio(const std::string& command,
                                                   const std::vector<std::string>& args,
                                                   MCPStdioFraming framing) {
    auto tr = std::make_unique<StdioMCPTransport>(
        command, args, std::map<std::string, std::string>{}, framing);
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
                                                   const std::map<std::string, std::string>& extra_env,
                                                   MCPStdioFraming framing) {
    auto tr = std::make_unique<StdioMCPTransport>(command, args, extra_env, framing);
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

namespace {

std::size_t mcp_resource_max_bytes() {
    constexpr std::size_t fallback = 256U * 1024U;
    const char* raw = std::getenv("AGENT_MCP_RESOURCE_MAX_BYTES");
    if (!raw || !*raw) return fallback;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (end == raw || *end != '\0' || parsed == 0 ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        return fallback;
    }
    return static_cast<std::size_t>(parsed);
}

std::string optional_string(const json& object, const char* key) {
    if (!object.contains(key) || object[key].is_null()) return {};
    if (!object[key].is_string())
        throw std::runtime_error(std::string("MCP resources: ") + key + " must be a string");
    return object[key].get<std::string>();
}

} // namespace

bool MCPClient::supports_resources() const noexcept {
    return resources_supported_;
}

bool MCPClient::supports_tools() const noexcept {
    return tools_supported_;
}

std::future<MCPResourceListResult> MCPClient::list_resources(
    std::string cursor, std::function<bool()> cancellation_requested) {
    return std::async(std::launch::async,
                      [this, cursor = std::move(cursor),
                       cancellation_requested = std::move(cancellation_requested)]() {
        if (!resources_supported_)
            throw std::runtime_error("mcp_resources_unsupported");
        json params = json::object();
        if (!cursor.empty()) params["cursor"] = cursor;
        const json result = send_jsonrpc_request(
            std::string(mcp_protocol::k_method_resources_list), params, cancellation_requested);
        if (!result.is_object() || !result.contains("resources") ||
            !result["resources"].is_array())
            throw std::runtime_error("MCP resources/list: resources must be an array");
        constexpr std::size_t max_resources_per_page = 10000U;
        if (result["resources"].size() > max_resources_per_page)
            throw std::runtime_error("MCP resources/list: resource page exceeds limit");
        MCPResourceListResult out;
        out.resources.reserve(result["resources"].size());
        for (const auto& item : result["resources"]) {
            if (!item.is_object() || !item.contains("uri") || !item["uri"].is_string() ||
                !item.contains("name") || !item["name"].is_string())
                throw std::runtime_error("MCP resources/list: resource requires string uri and name");
            MCPResource resource;
            resource.uri = item["uri"].get<std::string>();
            resource.name = item["name"].get<std::string>();
            if (resource.uri.empty() || resource.name.empty())
                throw std::runtime_error("MCP resources/list: resource uri and name must be non-empty");
            resource.description = optional_string(item, "description");
            resource.mime_type = optional_string(item, "mimeType");
            if (item.contains("size") && !item["size"].is_null()) {
                if (!item["size"].is_number_unsigned() && !item["size"].is_number_integer())
                    throw std::runtime_error("MCP resources/list: size must be a non-negative integer");
                if (item["size"].is_number_unsigned()) {
                    resource.size = item["size"].get<std::uint64_t>();
                } else {
                    const auto value = item["size"].get<std::int64_t>();
                    if (value < 0)
                        throw std::runtime_error("MCP resources/list: size must be non-negative");
                    resource.size = static_cast<std::uint64_t>(value);
                }
            }
            out.resources.push_back(std::move(resource));
        }
        if (result.contains("nextCursor") && !result["nextCursor"].is_null()) {
            if (!result["nextCursor"].is_string())
                throw std::runtime_error("MCP resources/list: nextCursor must be a string");
            std::string next = result["nextCursor"].get<std::string>();
            if (next.size() > 8192U)
                throw std::runtime_error("MCP resources/list: nextCursor exceeds limit");
            if (!next.empty()) out.next_cursor = std::move(next);
        }
        return out;
    });
}

std::future<std::vector<MCPResourceContent>> MCPClient::read_resource(
    std::string uri, std::function<bool()> cancellation_requested) {
    return std::async(std::launch::async,
                      [this, uri = std::move(uri),
                       cancellation_requested = std::move(cancellation_requested)]() {
        if (!resources_supported_)
            throw std::runtime_error("mcp_resources_unsupported");
        if (uri.empty()) throw std::invalid_argument("mcp_resource_uri_empty");
        const json result = send_jsonrpc_request(
            std::string(mcp_protocol::k_method_resources_read), json{{"uri", uri}},
            cancellation_requested);
        if (!result.is_object() || !result.contains("contents") ||
            !result["contents"].is_array())
            throw std::runtime_error("MCP resources/read: contents must be an array");
        if (result["contents"].size() > 1024U)
            throw std::runtime_error("MCP resources/read: content count exceeds limit");
        const std::size_t max_bytes = mcp_resource_max_bytes();
        std::size_t total = 0;
        std::vector<MCPResourceContent> out;
        out.reserve(result["contents"].size());
        for (const auto& item : result["contents"]) {
            if (!item.is_object() || !item.contains("uri") || !item["uri"].is_string())
                throw std::runtime_error("MCP resources/read: content requires string uri");
            const bool has_text = item.contains("text") && item["text"].is_string();
            const bool has_blob = item.contains("blob") && item["blob"].is_string();
            if (has_text == has_blob)
                throw std::runtime_error(
                    "MCP resources/read: content requires exactly one string text or blob");
            MCPResourceContent content;
            content.uri = item["uri"].get<std::string>();
            content.mime_type = optional_string(item, "mimeType");
            if (has_text) content.text = item["text"].get<std::string>();
            else content.blob = item["blob"].get<std::string>();
            const std::size_t bytes = has_text ? content.text->size() : content.blob->size();
            if (total > max_bytes || bytes > max_bytes - total)
                throw std::runtime_error("mcp_resource_too_large");
            total += bytes;
            out.push_back(std::move(content));
        }
        return out;
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
