/**
 * @file test_agent_server_auth_wp25.cpp
 * @brief AgentServer builtin AuthGate: JSON-RPC 401 + WWW-Authenticate; SSE 401 (WP2.5)
 */
#include <agent/agent_server/agent_server.hpp>
#include <agent/core/types.hpp>
#include "support/test_execution_profile.hpp"

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <thread>

#if __has_include(<httplib/httplib.hpp>)
#include <httplib/httplib.hpp>
#elif __has_include(<httplib.hpp>)
#include <httplib.hpp>
#else
#include <httplib.h>
#endif

namespace {

using agent_framework::AgentCard;
using agent_framework::AgentMessage;
using agent_framework::AgentPart;
using agent_framework::AgentServer;
using agent_framework::AgentTask;
using agent_framework::AgentTaskStatus;
using agent_framework::json;

bool wait_bound(AgentServer& s, int max_ms) {
    const auto t0 = std::chrono::steady_clock::now();
    while (s.bound_port() <= 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(max_ms)) {
            return false;
        }
    }
    return true;
}

void fail(const char* m) {
    std::cerr << "test_agent_server_auth_wp25: " << m << "\n";
    std::exit(1);
}

} // namespace

int main() {
#if defined(_WIN32)
    (void)_putenv_s("AGENT_SERVER_AUTH_MODE", "bearer");
    (void)_putenv_s("AGENT_SERVER_BEARER_TOKEN", "wp25-secret");
    (void)_putenv_s("AGENT_SERVER_BIND", "127.0.0.1");
    (void)_putenv_s("AGENT_SERVER_JSON_RPC_PATH", "/rpc");
    (void)_putenv_s("AGENT_SERVER_LEGACY_REST", "0");
#else
    (void)::setenv("AGENT_SERVER_AUTH_MODE", "bearer", 1);
    (void)::setenv("AGENT_SERVER_BEARER_TOKEN", "wp25-secret", 1);
    (void)::setenv("AGENT_SERVER_BIND", "127.0.0.1", 1);
    (void)::setenv("AGENT_SERVER_JSON_RPC_PATH", "/rpc", 1);
    (void)::setenv("AGENT_SERVER_LEGACY_REST", "0", 1);
#endif

    AgentServer server(0);
    AgentCard card;
    card.name = "wp25";
    card.description = "t";
    card.provider = "t";
    card.api_endpoint = "http://127.0.0.1:0/rpc";
    server.register_agent_card(card);
    agent_framework::test::configure_execution_profile(
        server, [](const agent_framework::RenderedPrompt&) { return "ok"; });

    std::thread th([&] { server.start(); });
    if (!wait_bound(server, 5000)) {
        server.stop();
        th.join();
        fail("bind");
    }
    const int port = server.bound_port();

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(30, 0);

    json rpc = json::parse(
        R"({"jsonrpc":"2.0","method":"SendMessage","id":1,"params":{"message":{"messageId":"m1","role":"ROLE_USER","parts":[{"type":"text","text":"hi"}]}}})");

    auto res_no = cli.Post("/rpc", rpc.dump(), "application/json");
    if (!res_no || res_no->status != 401) {
        server.stop();
        th.join();
        fail("expected 401 without auth");
    }
    if (res_no->get_header_value("WWW-Authenticate").empty()) {
        server.stop();
        th.join();
        fail("expected WWW-Authenticate on 401");
    }

    httplib::Headers h_ok;
    h_ok.insert({"Authorization", "Bearer wp25-secret"});
    auto res_ok = cli.Post("/rpc", h_ok, rpc.dump(), "application/json");
    if (!res_ok || res_ok->status != 200) {
        server.stop();
        th.join();
        fail("expected 200 with bearer");
    }

    json body = json::parse(res_ok->body);
    if (!body.contains("result")) {
        server.stop();
        th.join();
        fail("expected jsonrpc result");
    }
    std::string task_id = body["result"]["task"]["id"].get<std::string>();

    const std::string sse_path = "/tasks/sendSubscribe?task_id=" + task_id;
    auto res_sse_bad = cli.Get(sse_path.c_str());
    if (!res_sse_bad || res_sse_bad->status != 401) {
        server.stop();
        th.join();
        fail("expected SSE 401 without auth");
    }
    // Authenticated SSE opens a long-lived stream; I-3 satisfied by 401-without-auth above.

    server.stop();
    th.join();

#if defined(_WIN32)
    (void)_putenv_s("AGENT_SERVER_AUTH_MODE", "");
    (void)_putenv_s("AGENT_SERVER_BEARER_TOKEN", "");
#else
    (void)::unsetenv("AGENT_SERVER_AUTH_MODE");
    (void)::unsetenv("AGENT_SERVER_BEARER_TOKEN");
#endif

    std::cout << "test_agent_server_auth_wp25: ok\n";
    return 0;
}
