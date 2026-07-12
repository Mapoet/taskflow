/**
 * @file test_a2a_tier_e.cpp
 * @brief Tier E: light QPS + oversize POST + optional auth negative (in-process server)
 */
#include <agent/agent_server.hpp>
#include <agent/a2a/auth_gate.hpp>
#include <agent/types.hpp>
#include "support/test_execution_profile.hpp"

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <map>
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
using agent_framework::AgentServer;
using agent_framework::AgentTask;
using agent_framework::AgentTaskStatus;

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
    std::cerr << "test_a2a_tier_e: " << m << "\n";
    std::exit(1);
}

} // namespace

int main() {
    const char* g = std::getenv("AGENT_A2A_TIER_E");
    if (g == nullptr || g[0] == '\0' || g[0] == '0') {
        std::cout << "test_a2a_tier_e: SKIP (set AGENT_A2A_TIER_E=1)\n";
        return 0;
    }

    ::setenv("AGENT_SERVER_BIND", "127.0.0.1", 1);
    ::setenv("AGENT_SERVER_JSON_RPC_PATH", "/rpc", 1);
    ::setenv("AGENT_SERVER_LEGACY_REST", "0", 1);
    ::setenv("AGENT_A2A_STRICT", "1", 1);
    ::setenv("AGENT_SERVER_SSE_PING_SEC", "0", 1);
    ::setenv("AGENT_SERVER_WORKER_THREADS", "4", 1);
    ::setenv("AGENT_SERVER_EXECUTOR_THREADS", "2", 1);
    ::setenv("AGENT_SERVER_MAX_QUEUED_TASKS", "256", 1);

    AgentServer server(0);
    AgentCard card;
    card.name = "tier-e";
    card.description = "e";
    card.provider = "t";
    card.api_endpoint = "http://127.0.0.1:0/rpc";
    server.register_agent_card(card);
    agent_framework::test::configure_execution_profile(server);

    const char* auth_tok = std::getenv("AGENT_A2A_TIER_E_AUTH_TOKEN");
    if (auth_tok != nullptr && auth_tok[0]) {
        server.set_authentication_validator(
            [auth_tok](const agent_framework::a2a::AuthContext& ctx) {
                auto it = ctx.headers_lower.find("authorization");
                if (it == ctx.headers_lower.end()) {
                    return false;
                }
                return it->second == (std::string("Bearer ") + auth_tok);
            });
    }

    std::thread th([&] { server.start(); });
    if (!wait_bound(server, 5000)) {
        server.stop();
        th.join();
        fail("bind");
    }
    const int port = server.bound_port();
    card.api_endpoint = "http://127.0.0.1:" + std::to_string(port) + "/rpc";
    server.register_agent_card(card);

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(10, 0);

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        auto res = cli.Get("/.well-known/agent-card.json");
        if (!res || res->status != 200) {
            server.stop();
            th.join();
            fail("well-known qps");
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const auto ms_wk =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cerr << "test_a2a_tier_e: well-known x100 in " << ms_wk << " ms\n";

    json req = json::parse(
        R"({"jsonrpc":"2.0","method":"SendMessage","id":1,"params":{"message":{"messageId":"e","role":"ROLE_USER","parts":[]}}})");
    httplib::Headers hdr_ok;
    const bool need_auth = (auth_tok != nullptr && auth_tok[0]);
    if (need_auth) {
        hdr_ok.insert({"Authorization", std::string("Bearer ") + auth_tok});
    }
    const auto t2 = std::chrono::steady_clock::now();
    for (int j = 0; j < 100; ++j) {
        json r = req;
        r["id"] = j + 1;
        httplib::Result res = need_auth
                                  ? cli.Post("/rpc", hdr_ok, r.dump(), "application/json")
                                  : cli.Post("/rpc", r.dump(), "application/json");
        if (!res || res->status != 200) {
            server.stop();
            th.join();
            fail("send qps");
        }
    }
    const auto t3 = std::chrono::steady_clock::now();
    const auto ms_send =
        std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
    std::cerr << "test_a2a_tier_e: SendMessage x100 in " << ms_send << " ms\n";

    std::string big(8 * 1024 * 1024, 'x');
    big[0] = '{';
    httplib::Result res_big =
        need_auth ? cli.Post("/rpc", hdr_ok, big, "application/json") : cli.Post("/rpc", big, "application/json");
    if (!res_big) {
        server.stop();
        th.join();
        fail("huge post transport");
    }
    if (res_big->status != 200) {
        server.stop();
        th.join();
        fail("huge post status");
    }

    if (auth_tok != nullptr && auth_tok[0]) {
        json bad_auth_req = req;
        bad_auth_req["id"] = 99999;
        httplib::Headers h;
        h.insert({"Authorization", "Bearer wrong-token"});
        auto res401 = cli.Post("/rpc", h, bad_auth_req.dump(), "application/json");
        if (!res401 || res401->status != 401) {
            server.stop();
            th.join();
            fail("expected 401 for bad bearer");
        }
        std::string body = res401->body;
        if (body.find("stack") != std::string::npos || body.find("Stack") != std::string::npos) {
            server.stop();
            th.join();
            fail("401 body should not leak stack-like text");
        }
    }

    server.stop();
    th.join();
    std::cout << "test_a2a_tier_e: ok\n";
    return 0;
}
