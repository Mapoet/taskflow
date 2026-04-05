/**
 * @file test_a2a_orchestrator_tools.cpp
 * @brief ToolBus `a2a.send_message` + A2aPeerRegistry loopback (WP2.agent2agent, no external net)
 */
#include <agent/a2a/orchestration.hpp>
#include <agent/a2a/peer_registry.hpp>
#include <agent/agent_server.hpp>
#include <agent/toolbus.hpp>
#include <agent/types.hpp>

#include <chrono>
#include <cstdlib>
#include <future>
#ifdef _WIN32
#include <stdlib.h>
#endif
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using json = nlohmann::json;
using agent_framework::AgentCard;
using agent_framework::AgentMessage;
using agent_framework::AgentPart;
using agent_framework::AgentServer;
using agent_framework::AgentTask;
using agent_framework::AgentTaskStatus;
using agent_framework::ToolBus;
using agent_framework::a2a::A2aPeerRegistry;
using agent_framework::a2a::A2aToolRegistrationOptions;
using agent_framework::a2a::PeerSessionBook;
using agent_framework::a2a::kA2aOrchestratorToolSendMessage;
using agent_framework::a2a::register_a2a_orchestrator_tools;

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
    std::cerr << "test_a2a_orchestrator_tools: " << m << "\n";
    std::exit(1);
}

void test_send_message_tool() {
#if defined(_WIN32)
    (void)_putenv_s("AGENT_TOOL_ALLOWLIST", "");
#else
    (void)::setenv("AGENT_TOOL_ALLOWLIST", "", 1);
#endif
    (void)::setenv("AGENT_SERVER_BIND", "127.0.0.1", 1);
    (void)::setenv("AGENT_SERVER_JSON_RPC_PATH", "/rpc", 1);
    (void)::setenv("AGENT_SERVER_LEGACY_REST", "0", 1);

    auto inbound_session_ids = std::make_shared<std::vector<std::optional<std::string>>>();

    AgentServer server(0);
    AgentCard card;
    card.name = "orch-tool-test";
    card.description = "t";
    card.provider = "test";
    card.capabilities = {}; // no streaming → poll path
    card.api_endpoint = "http://127.0.0.1:0/rpc";
    server.register_agent_card(card);

    server.set_task_handler(
        [inbound_session_ids](AgentTask t,
                             std::shared_ptr<workflow::GraphBuilder>,
                             std::shared_ptr<agent_framework::TaskControl>) {
            return std::async(std::launch::async, [t, inbound_session_ids]() mutable {
                inbound_session_ids->push_back(t.session_id);
                AgentMessage reply;
                reply.role = AgentMessage::Role::AGENT;
                AgentPart part;
                part.type = AgentPart::Type::TEXT;
                part.text = std::string("ok");
                reply.parts.push_back(std::move(part));
                t.messages.push_back(std::move(reply));
                t.status = AgentTaskStatus::COMPLETED;
                t.session_id = std::string("ctx-loopback");
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
        {"id", "worker"},
        {"origin", origin},
        {"default_timeout_ms", 30000},
    }});

    A2aPeerRegistry registry;
    registry.load_from_json(peers_file);
    registry.discover_all();

    ToolBus bus;
    PeerSessionBook book;
    A2aToolRegistrationOptions reg_opts;
    reg_opts.default_timeout_ms = 0;
    register_a2a_orchestrator_tools(bus, registry, book, reg_opts);

    json args;
    args["peer_id"] = "worker";
    args["user_text"] = "hello";
    args["continue_session"] = true;

    json r = bus.call_tool(kA2aOrchestratorToolSendMessage, args).get();
    if (!r.value("ok", false)) {
        fail("first call not ok");
    }
    if (r.value("context_id", json()) != "ctx-loopback") {
        fail("context_id not propagated");
    }
    if (inbound_session_ids->size() != 1U) {
        fail("expected one handler invocation");
    }
    if (inbound_session_ids->at(0).has_value()) {
        fail("first call should not send session id");
    }

    json args2;
    args2["peer_id"] = "worker";
    args2["user_text"] = "again";
    args2["continue_session"] = true;
    json r2 = bus.call_tool(kA2aOrchestratorToolSendMessage, args2).get();
    if (!r2.value("ok", false)) {
        fail("second call not ok");
    }
    if (inbound_session_ids->size() != 2U) {
        fail("expected two handler invocations");
    }
    if (!inbound_session_ids->at(1).has_value() || *inbound_session_ids->at(1) != "ctx-loopback") {
        fail("continue_session should pass context id");
    }

    server.stop();
    th.join();
}

} // namespace

int main() {
    test_send_message_tool();
    return 0;
}
