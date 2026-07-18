#include <agent/internal/stdio_framing.hpp>
#include <agent/mcp_client/mcp_client.hpp>
#include <agent/toolbus/toolbus.hpp>

#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

using json = nlohmann::json;
using agent_framework::MCPClient;
using agent_framework::MCPStdioFraming;
using agent_framework::ToolBus;

json response_for(const json& request) {
    const std::string method = request.value("method", "");
    if (!request.contains("id")) {
        return json();
    }
    const json id = request.at("id");
    if (method == "initialize") {
        return json{{"jsonrpc", "2.0"},
                    {"id", id},
                    {"result",
                     {{"protocolVersion", "2024-11-05"},
                      {"capabilities", json::object()},
                      {"serverInfo", {{"name", "fake-stdio-mcp"}, {"version", "1"}}}}}};
    }
    if (method == "tools/list") {
        return json{{"jsonrpc", "2.0"},
                    {"id", id},
                    {"result",
                     {{"tools",
                       json::array({{{"name", "echo"},
                                     {"description", "echo input"},
                                     {"inputSchema", {{"type", "object"}}}}})}}}};
    }
    if (method == "tools/call") {
        return json{{"jsonrpc", "2.0"},
                    {"id", id},
                    {"result",
                     {{"content", json::array({{{"type", "text"}, {"text", "ok"}}})},
                      {"isError", false}}}};
    }
    return json{{"jsonrpc", "2.0"},
                {"id", id},
                {"error", {{"code", -32601}, {"message", "unknown method"}}}};
}

int run_server(MCPStdioFraming framing) {
#if defined(_WIN32)
    (void)framing;
    return 2;
#else
    std::string pending;
    for (;;) {
        std::string text;
        try {
            if (framing == MCPStdioFraming::JsonLines) {
                text = agent_framework::internal::read_one_json_line_text(
                    STDIN_FILENO, pending, 60000, 16U * 1024U * 1024U);
            } else {
                text = agent_framework::internal::read_one_framed_body_text(
                    STDIN_FILENO, pending, 60000, 256U * 1024U);
            }
        } catch (const std::runtime_error& e) {
            if (std::string(e.what()).find("EOF") != std::string::npos) {
                return 0;
            }
            return 3;
        }
        const json response = response_for(json::parse(text));
        if (response.is_null()) {
            continue;
        }
        std::string body = response.dump();
        if (framing == MCPStdioFraming::JsonLines) {
            std::cout << body << '\n' << std::flush;
        } else {
            std::cout << "Content-Length: " << body.size() << "\r\n\r\n"
                      << body << std::flush;
        }
    }
#endif
}

void exercise_client(const std::string& executable, MCPStdioFraming framing,
                     const std::string& mode) {
    auto client = MCPClient::create_stdio(executable, {"--server", mode}, framing);
    const auto tools = client->list_tools().get();
    assert(tools.size() == 1U);
    assert(tools.front().name == "echo");
    const json result = client->call_tool("echo", json{{"value", 42}}).get();
    assert(result.at("content").at(0).at("text") == "ok");
    client->disconnect();
}

void exercise_toolbus_legacy_config(const std::string& executable) {
#if defined(_WIN32)
    (void)executable;
#else
    const std::string path =
        "/tmp/agent-framework-mcp-framing-" + std::to_string(::getpid()) + ".json";
    const json config =
        {{"mcpServers",
          {{"legacy",
            {{"command", executable},
             {"args", json::array({"--server", "legacy"})},
             {"framing", "content-length"}}}}}};
    {
        std::ofstream out(path);
        assert(out);
        out << config.dump(2);
    }
    ToolBus bus;
    const auto imported = bus.register_mcp_from_cursor_config(path, true);
    (void)std::remove(path.c_str());
    assert(imported.failures.empty());
    assert(imported.registered_services.size() == 1U);
    const auto names = bus.list_all_tools();
    assert(names.size() == 1U);
    assert(names.front() == "legacy__echo");
    const json result = bus.call_tool("legacy__echo", json::object()).get();
    assert(result.at("content").at(0).at("text") == "ok");
#endif
}

void exercise_toolbus_safe_root_and_service_filter(const std::string& executable) {
#if defined(_WIN32)
    (void)executable;
#else
    const std::string root = "/tmp/agent-framework-safe-root";
    assert(::setenv("AGENT_FS_ROOT", root.c_str(), 1) == 0);
    const std::string path =
        "/tmp/agent-framework-mcp-filter-" + std::to_string(::getpid()) + ".json";
    const json config =
        {{"mcpServers",
          {{"filesystem",
            {{"command", executable},
             {"args", json::array({"--server", "jsonl", "${AGENT_FS_ROOT}"})}}},
           {"python_execute", {{"command", "/definitely/not/a/server"}}}}}};
    {
        std::ofstream out(path);
        assert(out);
        out << config.dump(2);
    }
    ToolBus bus;
    const auto imported =
        bus.register_mcp_from_cursor_config(path, true, {"python_execute"});
    (void)std::remove(path.c_str());
    assert(imported.failures.empty());
    assert(imported.registered_services == std::vector<std::string>{"filesystem"});
    assert(imported.skipped_services == std::vector<std::string>{"python_execute"});
    assert(bus.list_all_tools() == std::vector<std::string>{"filesystem__echo"});
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--server") {
        if (argc >= 4) {
            const char* expected = std::getenv("AGENT_FS_ROOT");
            if (!expected || argv[3] != std::string(expected)) return 4;
        }
        return run_server(std::string(argv[2]) == "legacy" ? MCPStdioFraming::ContentLength
                                                           : MCPStdioFraming::JsonLines);
    }
    exercise_client(argv[0], MCPStdioFraming::JsonLines, "jsonl");
    exercise_client(argv[0], MCPStdioFraming::ContentLength, "legacy");
    exercise_toolbus_legacy_config(argv[0]);
    exercise_toolbus_safe_root_and_service_filter(argv[0]);
    std::cout << "test_mcp_stdio_transport: ok\n";
    return 0;
}
