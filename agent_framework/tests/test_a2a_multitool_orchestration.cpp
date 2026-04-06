/**
 * @file test_a2a_multitool_orchestration.cpp
 * @brief Parallel a2a_submit_task via execute_tool_calls_sequenced + wait (loopback)
 */
#include <agent/a2a/orchestration.hpp>
#include <agent/a2a/outbound_task_supervisor.hpp>
#include <agent/a2a/peer_registry.hpp>
#include <agent/agent_server.hpp>
#include <agent/toolbus.hpp>
#include <agent/types.hpp>

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <stdlib.h>
#endif

using json = nlohmann::json;
using agent_framework::AgentCard;
using agent_framework::AgentMessage;
using agent_framework::AgentPart;
using agent_framework::AgentServer;
using agent_framework::AgentTask;
using agent_framework::AgentTaskStatus;
using agent_framework::CallSpec;
using agent_framework::ToolBus;
using agent_framework::a2a::A2aPeerRegistry;
using agent_framework::a2a::A2aToolRegistrationOptions;
using agent_framework::a2a::kA2aToolSubmitTask;
using agent_framework::a2a::OutboundSessionPolicy;
using agent_framework::a2a::OutboundTaskSupervisor;
using agent_framework::a2a::PeerSessionBook;
using agent_framework::a2a::register_a2a_orchestrator_tools;
using agent_framework::AgentConfig;
using agent_framework::execute_tool_calls_sequenced;
using agent_framework::resolve_tool_orchestration_options;

static bool wait_bound(AgentServer& s, int max_ms) {
    const auto t0 = std::chrono::steady_clock::now();
    while (s.bound_port() <= 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(max_ms)) {
            return false;
        }
    }
    return true;
}

static void fail(const char* m) {
    std::cerr << "test_a2a_multitool_orchestration: " << m << "\n";
    std::exit(1);
}

int main() {
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
    card.name = "multi";
    card.provider = "test";
    card.capabilities = {};
    card.api_endpoint = "http://127.0.0.1:0/rpc";
    server.register_agent_card(card);

    server.set_task_handler(
        [](AgentTask t, std::shared_ptr<workflow::GraphBuilder>, std::shared_ptr<agent_framework::TaskControl>) {
            return std::async(std::launch::async, [t]() mutable {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                AgentMessage reply;
                reply.role = AgentMessage::Role::AGENT;
                AgentPart part;
                part.type = AgentPart::Type::TEXT;
                part.text = std::string("x");
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

    json peers_file;
    peers_file["peers"] = json::array({json{
        {"id", "w"},
        {"origin", std::string("http://127.0.0.1:") + std::to_string(port)},
        {"default_timeout_ms", 30000},
    }});

    A2aPeerRegistry registry;
    registry.load_from_json(peers_file);
    registry.discover_all();

    auto busp = std::make_shared<ToolBus>();
    PeerSessionBook book;
    A2aToolRegistrationOptions reg{};
    reg.default_timeout_ms = 15000;
    auto sup = std::make_shared<OutboundTaskSupervisor>(registry, book, OutboundSessionPolicy{});
    register_a2a_orchestrator_tools(*busp, registry, book, sup, reg);

    AgentConfig cfg;
    cfg.enable_parallel_a2a_submits = true;
    cfg.max_parallel_a2a_submits = 4;
    const auto orch = resolve_tool_orchestration_options(cfg);

    auto classify = [busp](std::string_view nm) {
        return busp->get_tool_meta(std::string(nm)).side_effect;
    };

    std::vector<CallSpec> calls;
    for (int k = 0; k < 2; ++k) {
        CallSpec c;
        c.name = kA2aToolSubmitTask;
        c.arguments = json{{"peer_id", "w"}, {"user_text", std::string("m") + std::to_string(k)}};
        calls.push_back(std::move(c));
    }

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<json> parts = execute_tool_calls_sequenced(busp, calls, orch, classify);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    if (parts.size() != 2U) {
        fail("part size");
    }
    if (!parts[0].value("ok", false) || !parts[1].value("ok", false)) {
        fail("submit not ok");
    }
    if (ms >= 120) {
        fail("expected parallel overlap wall clock");
    }

    std::string h0 = parts[0].at("local_handle").get<std::string>();
    std::string h1 = parts[1].at("local_handle").get<std::string>();
    json wargs;
    wargs["handles"] = json::array({h0, h1});
    wargs["timeout_ms"] = 10000;
    wargs["mode"] = "all";
    json wr = sup->tool_wait_tasks(wargs);
    if (!wr.at("results")[0].value("ok", false) || !wr.at("results")[1].value("ok", false)) {
        fail("wait not ok");
    }

    server.stop();
    th.join();
    std::cout << "test_a2a_multitool_orchestration: ok\n";
    return 0;
}
