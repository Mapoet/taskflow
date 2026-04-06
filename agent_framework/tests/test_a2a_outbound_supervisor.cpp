/**
 * @file test_a2a_outbound_supervisor.cpp
 * @brief OutboundTaskSupervisor submit + wait + cancel smoke (loopback server)
 */
#include <agent/a2a/orchestration.hpp>
#include <agent/a2a/outbound_task_supervisor.hpp>
#include <agent/a2a/peer_registry.hpp>
#include <agent/agent_server.hpp>
#include <agent/types.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <stdlib.h>
#endif

namespace {

using json = nlohmann::json;
using agent_framework::AgentCard;
using agent_framework::AgentMessage;
using agent_framework::AgentPart;
using agent_framework::AgentServer;
using agent_framework::AgentTask;
using agent_framework::AgentTaskStatus;
using agent_framework::a2a::A2aPeerRegistry;
using agent_framework::a2a::A2aToolRegistrationOptions;
using agent_framework::a2a::OutboundSessionPolicy;
using agent_framework::a2a::OutboundTaskSupervisor;
using agent_framework::a2a::PeerSessionBook;

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
    std::cerr << "test_a2a_outbound_supervisor: " << m << "\n";
    std::exit(1);
}

void test_submit_wait() {
#if defined(_WIN32)
    (void)_putenv_s("AGENT_TOOL_ALLOWLIST", "");
#else
    (void)::setenv("AGENT_TOOL_ALLOWLIST", "", 1);
#endif
    (void)::setenv("AGENT_SERVER_BIND", "127.0.0.1", 1);
    (void)::setenv("AGENT_SERVER_JSON_RPC_PATH", "/rpc", 1);
    (void)::setenv("AGENT_SERVER_LEGACY_REST", "0", 1);

    AgentServer server(0);
    AgentCard card;
    card.name = "sup-test";
    card.description = "t";
    card.provider = "test";
    card.capabilities = {};
    card.api_endpoint = "http://127.0.0.1:0/rpc";
    server.register_agent_card(card);

    server.set_task_handler(
        [](AgentTask t, std::shared_ptr<workflow::GraphBuilder>, std::shared_ptr<agent_framework::TaskControl>) {
            return std::async(std::launch::async, [t]() mutable {
                AgentMessage reply;
                reply.role = AgentMessage::Role::AGENT;
                AgentPart part;
                part.type = AgentPart::Type::TEXT;
                part.text = std::string("done");
                reply.parts.push_back(std::move(part));
                t.messages.push_back(std::move(reply));
                t.status = AgentTaskStatus::COMPLETED;
                t.updated_at = std::chrono::system_clock::now();
                return t;
            });
        });

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
        {"id", "w"},
        {"origin", origin},
        {"default_timeout_ms", 30000},
    }});

    A2aPeerRegistry registry;
    registry.load_from_json(peers_file);
    registry.discover_all();

    PeerSessionBook book;
    OutboundSessionPolicy pol;
    pol.max_concurrent_monitors = 32;
    auto sup = std::make_shared<OutboundTaskSupervisor>(registry, book, pol);
    A2aToolRegistrationOptions to{};
    to.default_timeout_ms = 5000;

    json args;
    args["peer_id"] = "w";
    args["user_text"] = "hello";
    args["monitor"] = true;
    json r = sup->tool_submit(args, to);
    if (!r.value("ok", false)) {
        fail("submit failed");
    }
    const std::string h = r.at("local_handle").get<std::string>();

    json wargs;
    wargs["handles"] = json::array({h});
    wargs["timeout_ms"] = 8000;
    wargs["mode"] = "all";
    json wr = sup->tool_wait_tasks(wargs);
    if (!wr.value("ok", false)) {
        fail("wait failed");
    }
    const auto& res = wr.at("results");
    if (!res.is_array() || res.size() != 1) {
        fail("results size");
    }
    if (!res[0].value("ok", false)) {
        fail("task not ok");
    }

    server.stop();
    th.join();
}

} // namespace

int main() {
    test_submit_wait();
    std::cout << "test_a2a_outbound_supervisor: ok\n";
    return 0;
}
