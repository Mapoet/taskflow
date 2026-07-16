/**
 * @file test_a2a_peer_registry.cpp
 * @brief Offline peers.json parsing + loopback discover for A2aPeerRegistry (WP2.agent2agent)
 */
#include <agent/a2a/peer_registry.hpp>
#include <agent/agent_server/agent_server.hpp>
#include "support/test_execution_profile.hpp"
#include <agent/core/types.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

using json = nlohmann::json;
using agent_framework::AgentCard;
using agent_framework::AgentServer;
using agent_framework::a2a::A2aPeerRegistry;
using agent_framework::a2a::split_json_rpc_url;

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
    std::cerr << "test_a2a_peer_registry: " << m << "\n";
    std::exit(1);
}

void test_split_json_rpc_url() {
    std::string b;
    std::string p;
    split_json_rpc_url("http://127.0.0.1:9001/rpc", b, p);
    if (b != "http://127.0.0.1:9001" || p != "/rpc") {
        fail("split_json_rpc_url /rpc");
    }
    split_json_rpc_url("https://example.com", b, p);
    if (b != "https://example.com" || p != "/") {
        fail("split_json_rpc_url no path");
    }
}

void test_load_invalid_root() {
    A2aPeerRegistry reg;
    try {
        reg.load_from_json(json::array());
        fail("expected invalid_argument for non-object root");
    } catch (const std::invalid_argument&) {
    }
    try {
        reg.load_from_json(json{{"peers", "x"}});
        fail("expected invalid_argument for non-array peers");
    } catch (const std::invalid_argument&) {
    }
}

void test_duplicate_peer_id() {
    A2aPeerRegistry reg;
    json j;
    j["peers"] = json::array({
        json{{"id", "a"}, {"origin", "http://127.0.0.1:1"}},
        json{{"id", "a"}, {"origin", "http://127.0.0.1:2"}},
    });
    try {
        reg.load_from_json(j);
        fail("expected duplicate id");
    } catch (const std::invalid_argument& e) {
        const std::string m = e.what();
        if (m.find("duplicate") == std::string::npos) {
            fail("duplicate error message should mention duplicate");
        }
    }
}

void test_discover_all_loopback() {
    (void)::setenv("AGENT_SERVER_BIND", "127.0.0.1", 1);
    (void)::setenv("AGENT_SERVER_JSON_RPC_PATH", "/rpc", 1);
    (void)::setenv("AGENT_SERVER_LEGACY_REST", "0", 1);

    AgentServer server(0);
    AgentCard card;
    card.name = "orch-registry-test";
    card.description = "t";
    card.provider = "test";
    card.capabilities = {};
    card.api_endpoint = "http://127.0.0.1:0/rpc";
    server.register_agent_card(card);
    agent_framework::test::configure_execution_profile(server);

    std::thread th([&] { server.start(); });
    if (!wait_bound(server, 5000)) {
        server.stop();
        th.join();
        fail("bound port timeout");
    }
    const int port = server.bound_port();
    card.api_endpoint = "http://127.0.0.1:" + std::to_string(port) + "/rpc";
    server.register_agent_card(card);

    const std::string origin = "http://127.0.0.1:" + std::to_string(port);
    json peers_file;
    peers_file["peers"] = json::array({json{
        {"id", "worker"},
        {"origin", origin},
        {"well_known_path", "/.well-known/agent-card.json"},
    }});

    A2aPeerRegistry reg;
    reg.load_from_json(peers_file);
    reg.discover_all();
    if (reg.size() != 1U) {
        server.stop();
        th.join();
        fail("expected one resolved peer");
    }
    if (reg.card("worker").name != "orch-registry-test") {
        server.stop();
        th.join();
        fail("card name mismatch");
    }
    if (reg.default_timeout_ms_for("worker") <= 0) {
        server.stop();
        th.join();
        fail("default timeout");
    }

    server.stop();
    th.join();
}

} // namespace

int main() {
    test_split_json_rpc_url();
    test_load_invalid_root();
    test_duplicate_peer_id();
    test_discover_all_loopback();
    return 0;
}
