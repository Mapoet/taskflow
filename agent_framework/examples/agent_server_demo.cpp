/**
 * @file agent_server_demo.cpp
 * @brief Minimal AgentServer binary for Tier C/D live tests (WP2.a2a-test)
 */
#include <agent/agent_server.hpp>
#include <agent/types.hpp>

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <map>
#include <string>
#include <thread>

namespace {

using agent_framework::AgentCard;
using agent_framework::AgentMessage;
using agent_framework::AgentPart;
using agent_framework::AgentServer;
using agent_framework::AgentSkill;
using agent_framework::AgentTask;
using agent_framework::AgentTaskStatus;

std::string demo_role() {
    const char* r = std::getenv("AGENT_SERVER_DEMO_ROLE");
    return r ? std::string(r) : std::string("echo");
}

std::string card_name_for_role(const std::string& role) {
    if (role == "integration-worker") {
        return "integration-worker";
    }
    if (role == "integration-reviewer") {
        return "integration-reviewer";
    }
    return "agent-server-demo";
}

std::string json_rpc_path_normalized() {
    const char* o = std::getenv("AGENT_SERVER_JSON_RPC_PATH");
    std::string p = (o && o[0]) ? std::string(o) : std::string("/rpc");
    if (p[0] != '/') {
        p.insert(p.begin(), '/');
    }
    return p;
}

void build_card(AgentCard& card, const std::string& public_base, const std::string& jpath) {
    const std::string role = demo_role();
    card.name = card_name_for_role(role);
    card.description = "agent_server_demo (WP2.a2a-test)";
    card.provider = "taskflow";
    std::string base = public_base;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    card.api_endpoint = base + jpath;
    card.capabilities.push_back("streaming");

    AgentSkill sk;
    sk.description = "demo skill";
    sk.input_schema = json::object();
    sk.output_schema = json::object();
    if (role == "integration-worker") {
        sk.name = "skill.execute";
        sk.required_capabilities = {"execute"};
    } else if (role == "integration-reviewer") {
        sk.name = "skill.review";
        sk.required_capabilities = {"review"};
    } else {
        sk.name = "demo.echo";
        sk.required_capabilities = {"echo"};
    }
    card.skills.push_back(std::move(sk));
}

} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    if (!std::getenv("AGENT_SERVER_BIND")) {
        ::setenv("AGENT_SERVER_BIND", "127.0.0.1", 1);
    }
    const char* bind = std::getenv("AGENT_SERVER_BIND");
    ::setenv("AGENT_SERVER_LEGACY_REST", "0", 1);
    ::setenv("AGENT_A2A_STRICT", "1", 1);
    if (!std::getenv("AGENT_SERVER_SSE_PING_SEC")) {
        ::setenv("AGENT_SERVER_SSE_PING_SEC", "0", 1);
    }
    if (!std::getenv("AGENT_SERVER_WORKER_THREADS")) {
        ::setenv("AGENT_SERVER_WORKER_THREADS", "4", 1);
    }
    if (!std::getenv("AGENT_SERVER_EXECUTOR_THREADS")) {
        ::setenv("AGENT_SERVER_EXECUTOR_THREADS", "2", 1);
    }

    const std::string jpath = json_rpc_path_normalized();
    ::setenv("AGENT_SERVER_JSON_RPC_PATH", jpath.c_str(), 1);

    int port = 8080;
    if (const char* pe = std::getenv("AGENT_SERVER_PORT")) {
        if (pe[0]) {
            port = std::atoi(pe);
        }
    }

    std::string public_base = "http://";
    if (const char* pb = std::getenv("AGENT_SERVER_CARD_PUBLIC_BASE")) {
        public_base = pb;
    } else {
        public_base += bind;
        public_base += ":";
        public_base += std::to_string(port);
    }

    AgentServer server(port);
    AgentCard card;
    build_card(card, public_base, jpath);
    server.register_agent_card(card);

    const std::string role = demo_role();
    server.set_task_handler(
        [role](AgentTask t,
               std::shared_ptr<workflow::GraphBuilder>,
               std::shared_ptr<agent_framework::TaskControl>) {
            return std::async(std::launch::async, [t, role]() mutable {
                if (role == "integration-worker") {
                    AgentMessage reply;
                    reply.role = AgentMessage::Role::AGENT;
                    AgentPart p;
                    p.type = AgentPart::Type::TEXT;
                    p.text = std::string(R"({"signature":"SIG_TIER_D_A"})");
                    reply.parts.push_back(std::move(p));
                    t.messages.push_back(std::move(reply));
                    t.status = AgentTaskStatus::COMPLETED;
                    t.updated_at = std::chrono::system_clock::now();
                    return t;
                }
                if (role == "integration-reviewer") {
                    std::string blob;
                    for (const auto& m : t.messages) {
                        for (const auto& p : m.parts) {
                            if (p.type == AgentPart::Type::TEXT && p.text.has_value()) {
                                blob += *p.text;
                            }
                        }
                    }
                    if (blob.find("SIG_TIER_D_A") != std::string::npos) {
                        t.status = AgentTaskStatus::COMPLETED;
                    } else {
                        t.status = AgentTaskStatus::FAILED;
                    }
                    t.updated_at = std::chrono::system_clock::now();
                    return t;
                }
                t.status = AgentTaskStatus::COMPLETED;
                t.updated_at = std::chrono::system_clock::now();
                return t;
            });
        });

    const char* token = std::getenv("AGENT_SERVER_AUTH_TOKEN");
    if (token && token[0]) {
        server.set_authentication_validator(
            [token](const std::map<std::string, std::string>& headers) {
                auto it = headers.find("Authorization");
                if (it == headers.end()) {
                    return false;
                }
                const std::string expect = std::string("Bearer ") + token;
                return it->second == expect;
            });
    }

    std::cerr << "agent_server_demo: listening role=" << role << " card.url=" << card.api_endpoint
              << "\n";
    server.start();
    return 0;
}
