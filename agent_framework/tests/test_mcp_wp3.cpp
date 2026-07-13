/**
 * @file test_mcp_wp3.cpp
 * @brief WP1.3 MCP JSON-RPC、Mock 传输、ToolBus register_mcp_service
 */

#include <agent/mcp_client.hpp>
#include <agent/toolbus.hpp>

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <agent/internal/http_sse.hpp>
#include <agent/internal/stdio_framing.hpp>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

using json = nlohmann::json;
using namespace agent_framework;

std::string env_or_empty(const char* key) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : std::string();
}

std::string first_non_empty(std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        std::string v = env_or_empty(k);
        if (!v.empty()) {
            return v;
        }
    }
    return {};
}

std::optional<json> load_json_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return std::nullopt;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        return json::parse(ss.str());
    } catch (...) {
        return std::nullopt;
    }
}

struct CursorMcpServer {
    std::string name;
    std::string type; // "http" or "stdio" (best-effort)
    std::string url;
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> headers;
};

std::optional<CursorMcpServer> load_cursor_mcp_server(const std::string& server_name,
                                                      const std::string& mcp_json_path) {
    auto doc_opt = load_json_file(mcp_json_path);
    if (!doc_opt.has_value()) {
        return std::nullopt;
    }
    const json& doc = *doc_opt;
    if (!doc.contains("mcpServers") || !doc["mcpServers"].is_object()) {
        return std::nullopt;
    }
    const json& servers = doc["mcpServers"];
    if (!servers.contains(server_name) || !servers[server_name].is_object()) {
        return std::nullopt;
    }
    const json& s = servers[server_name];
    CursorMcpServer out;
    out.name = server_name;
    if (s.contains("type") && s["type"].is_string()) {
        out.type = s["type"].get<std::string>();
    }
    if (s.contains("url") && s["url"].is_string()) {
        out.url = s["url"].get<std::string>();
        if (out.type.empty()) {
            out.type = "http";
        }
    }
    if (s.contains("command") && s["command"].is_string()) {
        out.command = s["command"].get<std::string>();
        if (out.type.empty()) {
            out.type = "stdio";
        }
    }
    if (s.contains("args") && s["args"].is_array()) {
        for (const auto& a : s["args"]) {
            if (a.is_string()) {
                out.args.push_back(a.get<std::string>());
            }
        }
    }
    if (s.contains("headers") && s["headers"].is_object()) {
        for (auto it = s["headers"].begin(); it != s["headers"].end(); ++it) {
            if (it.value().is_string()) {
                out.headers[it.key()] = it.value().get<std::string>();
            }
        }
    }
    if (out.type == "http" && out.url.empty()) {
        return std::nullopt;
    }
    if (out.type == "stdio" && out.command.empty()) {
        return std::nullopt;
    }
    return out;
}

class MockMcpTransport : public MCPTransportInterface {
public:
    struct ControlState {
        bool cancellable_called = false;
        bool cancellation_observed = false;
    };

    explicit MockMcpTransport(std::shared_ptr<ControlState> state = {}) : state_(std::move(state)) {}
    bool connect(const std::string& /*endpoint*/) override {
        connected_ = true;
        return true;
    }
    void disconnect() override {
        connected_ = false;
    }
    bool is_connected() const override {
        return connected_;
    }
    MCPTransport get_transport_type() const override {
        return MCPTransport::HTTP;
    }

    json transceive(const json& req) override {
        const std::string& method = req.at("method").get_ref<const std::string&>();
        std::int64_t id = req.at("id").get<std::int64_t>();
        if (method == "initialize") {
            return json{{"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", {{"protocolVersion", "2024-11-05"}, {"capabilities", json::object()}}}};
        }
        if (method == "tools/list") {
            return json{{"jsonrpc", "2.0"},
                        {"id", id},
                        {"result",
                         {{"tools",
                           json::array({{{"name", "echo"},
                                         {"description", "echo tool"},
                                         {"inputSchema",
                                          {{"type", "object"}, {"properties", json::object()}}}}})}}}};
        }
        if (method == "tools/call") {
            return json{{"jsonrpc", "2.0"},
                        {"id", id},
                        {"result",
                         {{"content", json::array({{{"type", "text"}, {"text", "42"}}})},
                          {"isError", false}}}};
        }
        return json{{"jsonrpc", "2.0"},
                    {"id", id},
                    {"error", {{"code", -32601}, {"message", "unknown method"}}}};
    }

    json transceive_cancellable(
        const json& req, const std::function<bool()>& cancellation_requested) override {
        if (state_) state_->cancellable_called = true;
        if (cancellation_requested && cancellation_requested()) {
            if (state_) state_->cancellation_observed = true;
            throw std::runtime_error("mock request cancelled");
        }
        return transceive(req);
    }

    void send_notification(const json& /*jsonrpc_notification*/) override {}

private:
    bool connected_ = false;
    std::shared_ptr<ControlState> state_;
};

void test_parse_jsonrpc() {
    json ok{{"jsonrpc", "2.0"}, {"id", 1}, {"result", {{"tools", json::array()}}}};
    json r = MCPClient::parse_jsonrpc_response(ok, 1);
    assert(r.contains("tools"));

    json err{{"jsonrpc", "2.0"}, {"id", 2}, {"error", {{"message", "fail"}, {"code", -1}}}};
    try {
        (void)MCPClient::parse_jsonrpc_response(err, 2);
        assert(false);
    } catch (const std::runtime_error&) {
    }

    json bad_id{{"jsonrpc", "2.0"}, {"id", 9}, {"result", json::object()}};
    try {
        (void)MCPClient::parse_jsonrpc_response(bad_id, 3);
        assert(false);
    } catch (const std::runtime_error&) {
    }
}

void test_toolbus_mcp_register_and_call() {
    auto client =
        MCPClient::create_with_transport(std::make_unique<MockMcpTransport>(), true);
    ToolBus bus;
    bus.register_mcp_service("svc", client);
    auto names = bus.list_all_tools();
    assert(names.size() == 1U);
    assert(names[0] == "svc__echo");

    json out = bus.call_tool("svc__echo", json::object()).get();
    assert(out.contains("content"));

    auto tools = bus.export_as_llm_tools();
    assert(tools.size() == 1U);
    assert(tools[0].name == "svc__echo");
}

void test_mcp_cancellation_contract() {
    auto state = std::make_shared<MockMcpTransport::ControlState>();
    auto client = MCPClient::create_with_transport(std::make_unique<MockMcpTransport>(state), true);
    bool cancelled = false;
    json out = client->call_tool("echo", json::object(), [&] { return cancelled; }).get();
    assert(out.contains("content"));
    assert(state->cancellable_called);
    assert(!state->cancellation_observed);

    cancelled = true;
    const json cancelled_result =
        client->call_tool("echo", json::object(), [&] { return cancelled; }).get();
    assert(cancelled_result.value("code", "") == "mcp_jsonrpc_error");
    assert(state->cancellation_observed);

    // Legacy transports that only override transceive retain the default adapter.
    auto legacy = MCPClient::create_with_transport(std::make_unique<MockMcpTransport>(), true);
    assert(legacy->call_tool("echo", json::object(), [] { return false; }).get().contains("content"));
}

void test_parse_sse_body_single_line() {
    const std::string body =
        "event: message\n"
        "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n"
        "\n";
    const std::string json_text = agent_framework::internal::parse_sse_body_to_json_text(body);
    json j = json::parse(json_text);
    assert(j.at("jsonrpc") == "2.0");
    assert(j.at("id") == 1);
}

void test_parse_sse_body_multi_line_concat() {
    const std::string body =
        "data: {\"jsonrpc\":\"2.0\",\n"
        "data:  \"id\":1,\n"
        "data:  \"result\":{}}\n"
        "\n";
    const std::string json_text = agent_framework::internal::parse_sse_body_to_json_text(body);
    json j = json::parse(json_text);
    assert(j.at("jsonrpc") == "2.0");
    assert(j.at("id") == 1);
    assert(j.contains("result"));
}

void test_parse_sse_body_done_stops() {
    const std::string body =
        "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n"
        "data: [DONE]\n"
        "data: {\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}\n";
    const std::string json_text = agent_framework::internal::parse_sse_body_to_json_text(body);
    json j = json::parse(json_text);
    assert(j.at("id") == 1);
}

void test_parse_sse_body_missing_data_throws() {
    const std::string body = "event: ping\n\n";
    try {
        (void)agent_framework::internal::parse_sse_body_to_json_text(body);
        assert(false);
    } catch (const std::runtime_error&) {
    }
}

void test_stdio_framing_noise_prefix_ok() {
#if defined(_WIN32)
    return;
#else
    int p[2]{-1, -1};
    assert(::pipe(p) == 0);
    const int rd = p[0];
    const int wr = p[1];

    const std::string body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}";
    std::ostringstream frame;
    frame << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    const std::string noise = "Context7 Documentation MCP Server v2.1.6 running on stdio\n";
    const std::string payload = noise + frame.str();

    (void)::write(wr, payload.data(), payload.size());
    ::close(wr);

    std::string pending;
    const std::string out = agent_framework::internal::read_one_framed_body_text(
        rd, pending, /*timeout_ms=*/2000, /*max_scan_bytes=*/256 * 1024);
    json j = json::parse(out);
    assert(j.at("id") == 1);
    ::close(rd);
#endif
}

void test_stdio_framing_noise_between_frames_ok() {
#if defined(_WIN32)
    return;
#else
    int p[2]{-1, -1};
    assert(::pipe(p) == 0);
    const int rd = p[0];
    const int wr = p[1];

    const std::string b1 = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}";
    const std::string b2 = "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}";
    std::ostringstream f1;
    f1 << "Content-Length: " << b1.size() << "\r\n\r\n" << b1;
    std::ostringstream f2;
    f2 << "Content-Length: " << b2.size() << "\r\n\r\n" << b2;
    const std::string noise = "banner line\n";
    const std::string payload = f1.str() + noise + f2.str();

    (void)::write(wr, payload.data(), payload.size());
    ::close(wr);

    std::string pending;
    const std::string o1 = agent_framework::internal::read_one_framed_body_text(
        rd, pending, 2000, 256 * 1024);
    const std::string o2 = agent_framework::internal::read_one_framed_body_text(
        rd, pending, 2000, 256 * 1024);
    json j1 = json::parse(o1);
    json j2 = json::parse(o2);
    assert(j1.at("id") == 1);
    assert(j2.at("id") == 2);
    ::close(rd);
#endif
}

void test_stdio_framing_only_noise_throws() {
#if defined(_WIN32)
    return;
#else
    int p[2]{-1, -1};
    assert(::pipe(p) == 0);
    const int rd = p[0];
    const int wr = p[1];
    const std::string payload(2048, 'x');
    (void)::write(wr, payload.data(), payload.size());
    ::close(wr);

    std::string pending;
    try {
        (void)agent_framework::internal::read_one_framed_body_text(
            rd, pending, 2000, /*max_scan_bytes=*/1024);
        assert(false);
    } catch (const std::runtime_error&) {
    }
    ::close(rd);
#endif
}

void test_stdio_framing_fragmented_header_ok() {
#if defined(_WIN32)
    return;
#else
    int p[2]{-1, -1};
    assert(::pipe(p) == 0);
    const int rd = p[0];
    const int wr = p[1];

    const std::string body = "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{}}";
    std::ostringstream hdr;
    hdr << "Content-Length: " << body.size() << "\r\n\r\n";
    const std::string h = hdr.str();

    (void)::write(wr, "Content-", 8);
    (void)::write(wr, "Length: ", 8);
    (void)::write(wr, h.substr(16).data(), h.size() - 16);
    (void)::write(wr, body.data(), body.size());
    ::close(wr);

    std::string pending;
    const std::string out = agent_framework::internal::read_one_framed_body_text(
        rd, pending, 2000, 256 * 1024);
    json j = json::parse(out);
    assert(j.at("id") == 7);
    ::close(rd);
#endif
}

int run_live_http() {
    std::string post_url = first_non_empty({"AGENT_MCP_HTTP_URL", "AGENT_MCP_HTTP_POST_URL"});
    std::map<std::string, std::string> headers;
    const std::string auth = env_or_empty("AGENT_MCP_AUTH_HEADER");
    if (!auth.empty()) {
        headers["Authorization"] = auth;
    }
    const std::string headers_json = env_or_empty("AGENT_MCP_HEADERS_JSON");
    if (!headers_json.empty()) {
        try {
            json h = json::parse(headers_json);
            if (!h.is_object()) {
                throw std::runtime_error("headers json is not object");
            }
            for (auto it = h.begin(); it != h.end(); ++it) {
                if (it.value().is_string()) {
                    headers[it.key()] = it.value().get<std::string>();
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "test_mcp_wp3 --live: invalid AGENT_MCP_HEADERS_JSON: " << e.what()
                      << "\n";
            return 2;
        }
    }

    std::shared_ptr<MCPClient> client;
    const std::string cursor_mcp_path = first_non_empty({"AGENT_MCP_CONFIG_PATH", "CURSOR_MCP_CONFIG_PATH"});
    const std::string cursor_server = first_non_empty({"AGENT_MCP_SERVER", "CURSOR_MCP_SERVER"});
    if (post_url.empty() && !cursor_mcp_path.empty() && !cursor_server.empty()) {
        auto s = load_cursor_mcp_server(cursor_server, cursor_mcp_path);
        if (!s.has_value()) {
            std::cerr << "test_mcp_wp3 --live: failed to load server \"" << cursor_server
                      << "\" from config (check AGENT_MCP_CONFIG_PATH)\n";
            return 2;
        }
        if (s->type != "http") {
            std::cerr << "test_mcp_wp3 --live: server \"" << cursor_server
                      << "\" is not http in config; set AGENT_MCP_HTTP_URL instead\n";
            return 2;
        }
        post_url = s->url;
        for (const auto& kv : s->headers) {
            headers.emplace(kv.first, kv.second);
        }
        std::cout << "test_mcp_wp3 --live: using Cursor MCP server \"" << cursor_server << "\"\n";
    }
    if (post_url.empty()) {
        std::cerr
            << "test_mcp_wp3 --live: missing AGENT_MCP_HTTP_URL (or AGENT_MCP_HTTP_POST_URL)\n"
            << "  Alternatively set:\n"
            << "    AGENT_MCP_CONFIG_PATH=/home/<user>/.cursor/mcp.json\n"
            << "    AGENT_MCP_SERVER=<serverName>\n";
        return 2;
    }
    try {
        client = MCPClient::create_http(post_url, headers);
    } catch (const std::exception& e) {
        std::cerr << "test_mcp_wp3 --live: MCPClient::create_http failed: " << e.what() << "\n";
        return 2;
    }

    std::vector<ToolMeta> metas;
    try {
        metas = client->list_tools().get();
    } catch (const std::exception& e) {
        std::cerr << "test_mcp_wp3 --live: list_tools failed: " << e.what() << "\n";
        return 2;
    }

    std::cout << "live tools/list (" << metas.size() << "):\n";
    for (const auto& m : metas) {
        std::cout << "  - " << m.name << "\n";
    }
    if (metas.empty()) {
        std::cerr << "test_mcp_wp3 --live: tools/list returned 0 tools\n";
        return 2;
    }

    ToolBus bus;
    try {
        bus.register_mcp_service("svc", client);
    } catch (const std::exception& e) {
        std::cerr << "test_mcp_wp3 --live: register_mcp_service failed: " << e.what() << "\n";
        return 2;
    }

    const std::string calls_raw = env_or_empty("AGENT_MCP_TEST_CALLS");
    if (calls_raw.empty()) {
        std::cout << "test_mcp_wp3 --live: no AGENT_MCP_TEST_CALLS provided; list-only mode ok\n";
        return 0;
    }

    json calls;
    try {
        calls = json::parse(calls_raw);
    } catch (const std::exception& e) {
        std::cerr << "test_mcp_wp3 --live: invalid AGENT_MCP_TEST_CALLS JSON: " << e.what()
                  << "\n";
        return 2;
    }
    if (!calls.is_array()) {
        std::cerr << "test_mcp_wp3 --live: AGENT_MCP_TEST_CALLS must be a JSON array\n";
        return 2;
    }

    for (const auto& c : calls) {
        if (!c.is_object() || !c.contains("name") || !c["name"].is_string()) {
            std::cerr << "test_mcp_wp3 --live: each call must have string field name\n";
            return 2;
        }
        const std::string remote_name = c["name"].get<std::string>();
        json args = json::object();
        if (c.contains("arguments")) {
            args = c["arguments"];
        }
        const std::string registered = std::string("svc__") + remote_name;
        json out = bus.call_tool(registered, args).get();
        std::cout << "call " << registered << " => " << out.dump() << "\n";
        if (out.is_object() && out.contains("code") && out["code"].is_string()) {
            const std::string code = out["code"].get<std::string>();
            if (code.rfind("mcp_", 0) == 0 || code == "tool_internal_error") {
                std::cerr << "test_mcp_wp3 --live: tool call failed: " << code << "\n";
                return 2;
            }
        }
    }

    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--live") {
        return run_live_http();
    }
    test_parse_jsonrpc();
    test_toolbus_mcp_register_and_call();
    test_mcp_cancellation_contract();
    test_parse_sse_body_single_line();
    test_parse_sse_body_multi_line_concat();
    test_parse_sse_body_done_stops();
    test_parse_sse_body_missing_data_throws();
    test_stdio_framing_noise_prefix_ok();
    test_stdio_framing_noise_between_frames_ok();
    test_stdio_framing_only_noise_throws();
    test_stdio_framing_fragmented_header_ok();
    std::cout << "test_mcp_wp3: all tests passed\n";
    return 0;
}
