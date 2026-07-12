/**
 * @file agent_server_demo.cpp
 * @brief Minimal AgentServer binary for Tier C/D live tests (WP2.a2a-test)
 */
#include <agent/agent_server.hpp>
#include <agent/a2a/auth_gate.hpp>
#include <agent/types.hpp>
#include <agent/llm_client.hpp>
#include <agent/prompt_renderer.hpp>
#include <agent/toolbus.hpp>

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

class DemoAdapter final : public agent_framework::ModelAdapter {
public:
    explicit DemoAdapter(std::string role) : role_(std::move(role)) {}
    std::future<agent_framework::LLMOutput> invoke(
        const agent_framework::LLMInput& input,
        std::function<void(std::string_view)> = nullptr) override {
        agent_framework::RenderedPrompt rendered;
        for (const auto& message : input.history) {
            rendered.messages.push_back({{"role", message.role}, {"content", message.content}});
        }
        return invoke_with_rendered(rendered);
    }
    std::future<agent_framework::LLMOutput> invoke_with_rendered(
        const agent_framework::RenderedPrompt& rendered,
        std::function<void(std::string_view)> = nullptr) override {
        std::promise<agent_framework::LLMOutput> promise;
        agent_framework::LLMOutput output;
        output.is_final = true;
        if (role_ == "integration-worker") {
            output.final_answer = R"({"signature":"SIG_TIER_D_A"})";
        } else if (role_ == "integration-reviewer") {
            bool found = false;
            for (const auto& message : rendered.messages) {
                found = found || message.value("content", "").find("SIG_TIER_D_A") != std::string::npos;
            }
            output.final_answer = found ? "review_pass" : "review_failed: missing signature";
        } else {
            output.final_answer = "ok";
        }
        promise.set_value(std::move(output));
        return promise.get_future();
    }
    std::vector<agent_framework::ToolMeta> get_available_tools() const override { return {}; }
    void configure(const agent_framework::ModelConfig&) override {}
    std::string get_model_name() const override { return "agent-server-demo"; }
    bool supports_multimodal() const override { return false; }
private:
    std::string role_;
};

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
    auto llm = std::make_shared<agent_framework::LLMClient>();
    llm->set_prompt_renderer(std::make_shared<agent_framework::PromptRenderer>());
    llm->register_adapter("demo", std::make_shared<DemoAdapter>(role));
    llm->set_default_adapter("demo");
    agent_framework::AgentExecutionProfile profile;
    profile.config.name = card.name;
    profile.config.system_prompt = "Execute the demo role deterministically.";
    profile.config.max_iterations = 2;
    profile.deps = {llm, std::make_shared<agent_framework::ToolBus>(), nullptr};
    server.set_execution_profile(std::move(profile));

    const char* token = std::getenv("AGENT_SERVER_AUTH_TOKEN");
    if (token && token[0]) {
        server.set_authentication_validator(
            [token](const agent_framework::a2a::AuthContext& ctx) {
                auto it = ctx.headers_lower.find("authorization");
                if (it == ctx.headers_lower.end()) {
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
