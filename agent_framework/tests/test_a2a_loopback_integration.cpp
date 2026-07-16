/**
 * @file test_a2a_loopback_integration.cpp
 * @brief Tier B: Well-Known → discover_agent → JSON-RPC + SSE (AGENT_A2A_INTEGRATION_LOOPBACK=1)
 */
#include <agent/agent_client/agent_client.hpp>
#include <agent/agent_server/agent_server.hpp>
#include <agent/a2a/client_config.hpp>
#include <agent/agent/task_state_machine.hpp>
#include <agent/core/types.hpp>
#include "support/test_execution_profile.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using agent_framework::AgentCard;
using agent_framework::AgentClient;
using agent_framework::AgentClientOptions;
using agent_framework::AgentMessage;
using agent_framework::AgentPart;
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

void fail(const char* msg) {
    std::cerr << "test_a2a_loopback_integration: " << msg << "\n";
    std::exit(1);
}

/** Split Card `url` / api_endpoint into JSON-RPC base URL and path (e.g. http://h:1/rpc → /rpc). */
void split_api_endpoint(const std::string& api_ep, std::string& out_base, std::string& out_path) {
    const std::size_t scheme = api_ep.find("://");
    if (scheme == std::string::npos) {
        fail("split_api_endpoint: missing scheme");
    }
    const std::size_t path_start = api_ep.find('/', scheme + 3);
    if (path_start == std::string::npos) {
        out_base = api_ep;
        out_path = "/";
        return;
    }
    out_base = api_ep.substr(0, path_start);
    out_path = api_ep.substr(path_start);
    if (out_path.empty()) {
        out_path = "/";
    }
}

void loopback_happy() {
    ::setenv("AGENT_SERVER_BIND", "127.0.0.1", 1);
    ::setenv("AGENT_SERVER_JSON_RPC_PATH", "/rpc", 1);
    ::setenv("AGENT_SERVER_LEGACY_REST", "0", 1);
    ::setenv("AGENT_A2A_STRICT", "1", 1);
    ::setenv("AGENT_SERVER_SSE_PING_SEC", "0", 1);
    ::setenv("AGENT_SERVER_WORKER_THREADS", "4", 1);
    ::setenv("AGENT_SERVER_EXECUTOR_THREADS", "2", 1);

    AgentServer server(0);
    AgentCard card;
    card.name = "loopback";
    card.description = "tier-b";
    card.provider = "test";
    card.capabilities.push_back("streaming");
    card.api_endpoint = "http://127.0.0.1:0/rpc";
    server.register_agent_card(card);

    agent_framework::test::configure_execution_profile(
        server, [](const agent_framework::RenderedPrompt& rendered) {
            if (agent_framework::test::rendered_contains(rendered, "sse")) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            return "ok";
        });

    std::thread th([&] { server.start(); });
    if (!wait_bound(server, 5000)) {
        server.stop();
        th.join();
        fail("bound port timeout");
    }
    const int port = server.bound_port();
    const std::string rpc_path = "/rpc";
    card.api_endpoint = "http://127.0.0.1:" + std::to_string(port) + rpc_path;
    server.register_agent_card(card);

    const std::string discover_base = "http://127.0.0.1:" + std::to_string(port);
    AgentClient discover_cli(discover_base, AgentClientOptions{});
    AgentCard remote = discover_cli.discover_agent("/.well-known/agent-card.json").get();
    if (remote.api_endpoint != card.api_endpoint) {
        server.stop();
        th.join();
        fail("Well-Known url mismatch vs registered api_endpoint");
    }

    std::string rpc_base;
    std::string path_from_card;
    split_api_endpoint(remote.api_endpoint, rpc_base, path_from_card);
    if (path_from_card != rpc_path) {
        server.stop();
        th.join();
        fail("card path mismatch");
    }

    AgentClientOptions rpc_opts;
    rpc_opts.use_legacy_rest = false;
    rpc_opts.json_rpc_path = path_from_card;
    AgentClient rpc_cli(rpc_base, rpc_opts);

    AgentMessage msg;
    msg.role = AgentMessage::Role::USER;
    AgentPart part;
    part.type = AgentPart::Type::TEXT;
    part.text = std::string("sse");
    msg.parts.push_back(std::move(part));

    AgentTask created = rpc_cli.send_task("", msg, std::nullopt, json::object()).get();
    if (created.task_id.empty()) {
        server.stop();
        th.join();
        fail("empty task id");
    }

    std::atomic<bool> saw_done{false};
    rpc_cli.subscribe_task_updates("", created.task_id,
                                   [&](const AgentTask& u) {
                                       if (u.status == AgentTaskStatus::COMPLETED) {
                                           saw_done = true;
                                       }
                                   },
                                   [](const agent_framework::AgentArtifact&) {});

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline && !saw_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!saw_done.load()) {
        server.stop();
        th.join();
        fail("SSE did not observe COMPLETED");
    }

    server.stop();
    th.join();
}

void loopback_concurrent() {
    ::setenv("AGENT_SERVER_BIND", "127.0.0.1", 1);
    ::setenv("AGENT_SERVER_JSON_RPC_PATH", "/rpc", 1);
    ::setenv("AGENT_SERVER_LEGACY_REST", "0", 1);
    ::setenv("AGENT_A2A_STRICT", "1", 1);
    ::setenv("AGENT_SERVER_SSE_PING_SEC", "0", 1);
    ::setenv("AGENT_SERVER_WORKER_THREADS", "8", 1);
    ::setenv("AGENT_SERVER_EXECUTOR_THREADS", "4", 1);

    AgentServer server(0);
    AgentCard card;
    card.name = "loopback-conc";
    card.description = "tier-b";
    card.provider = "test";
    card.api_endpoint = "http://127.0.0.1:0/rpc";
    server.register_agent_card(card);

    agent_framework::test::configure_execution_profile(server);

    std::thread th([&] { server.start(); });
    if (!wait_bound(server, 5000)) {
        server.stop();
        th.join();
        fail("concurrent: bound port timeout");
    }
    const int port = server.bound_port();
    card.api_endpoint = "http://127.0.0.1:" + std::to_string(port) + "/rpc";
    server.register_agent_card(card);

    const std::string rpc_base = "http://127.0.0.1:" + std::to_string(port);
    AgentClientOptions rpc_opts;
    rpc_opts.use_legacy_rest = false;
    rpc_opts.json_rpc_path = "/rpc";

    constexpr int N = 16;
    std::vector<std::thread> workers;
    std::atomic<int> successes{0};

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        workers.emplace_back([rpc_base, rpc_opts, &successes]() {
            AgentClient cli(rpc_base, rpc_opts);
            AgentMessage msg;
            msg.role = AgentMessage::Role::USER;
            AgentPart p;
            p.type = AgentPart::Type::TEXT;
            p.text = std::string("hi");
            msg.parts.push_back(std::move(p));
            try {
                AgentTask t = cli.send_task("", msg, std::nullopt, json::object()).get();
                for (int k = 0; k < 200; ++k) {
                    t = cli.get_task("", t.task_id).get();
                    if (t.status == AgentTaskStatus::COMPLETED) {
                        successes.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            } catch (...) {
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    server.stop();
    th.join();

    if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(60)) {
        fail("concurrent: T_multi exceeded");
    }
    if (successes.load() != N) {
        fail("concurrent: not all tasks completed");
    }
}

} // namespace

int main() {
    const char* gate = std::getenv("AGENT_A2A_INTEGRATION_LOOPBACK");
    if (gate == nullptr || gate[0] == '\0' || (gate[0] == '0' && gate[1] == '\0')) {
        std::cout << "test_a2a_loopback_integration: SKIP (set AGENT_A2A_INTEGRATION_LOOPBACK=1)\n";
        return 0;
    }
    ::setenv("AGENT_CLIENT_USE_LEGACY_REST", "0", 1);
    try {
        loopback_happy();
        loopback_concurrent();
    } catch (const std::exception& e) {
        std::cerr << "test_a2a_loopback_integration: exception: " << e.what() << "\n";
        return 1;
    }
    std::cout << "test_a2a_loopback_integration: ok\n";
    return 0;
}
