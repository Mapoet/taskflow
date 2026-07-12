/**
 * @file test_agent_server_wp27_input.cpp
 * @brief WP2.7：JSON-RPC SendMessage 严格模式下 /cmd 拒绝 → -32602 + violations
 */
#include <agent/agent_server.hpp>
#include <agent/a2a/wire_mapping.hpp>
#include <agent/execution_context.hpp>
#include <agent/task_state_machine.hpp>
#include <agent/types.hpp>
#include <agent/user_input_preprocessor.hpp>
#include <workflow/nodeflow.hpp>
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

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

bool wait_bound(agent_framework::AgentServer& s, int max_ms) {
    const auto t0 = std::chrono::steady_clock::now();
    while (s.bound_port() <= 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(max_ms)) {
            return false;
        }
    }
    return true;
}

void send_bad_cmd_expect_32602(int port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(5, 0);
    json req = json::parse(R"({
        "jsonrpc": "2.0",
        "method": "SendMessage",
        "id": 77,
        "params": {
            "message": {
                "messageId": "m-wp27",
                "role": "ROLE_USER",
                "parts": [{"text": "/not_whitelisted_cmd_xyz", "mediaType": "text/plain"}]
            }
        }
    })");

    {
        agent_framework::AgentMessage wm =
            agent_framework::a2a::message_from_a2a_wire(req["params"]["message"]);
        const std::string rw = agent_framework::concat_user_text_from_message(wm);
        if (rw.empty()) {
            throw std::runtime_error("wp27 wire concat empty (check A2A part wire shape)");
        }
        agent_framework::PreprocessOptions po;
        agent_framework::UserInputPreprocessor prep(po);
        agent_framework::ExecutionContext cx = agent_framework::ExecutionContext::from_environment();
        cx.input_policy_version = "wp27-v1";
        auto out = prep.process(rw, cx);
        if (out.tier_a_violations.empty()) {
            throw std::runtime_error(
                "wp27 preprocessor expected tier_a_violations (AGENT_INPUT_STRICT in this process?)");
        }
    }

    auto res = cli.Post("/rpc", req.dump(), "application/json");
    if (!res || res->status != 200) {
        throw std::runtime_error("wp27 post failed");
    }
    json body = json::parse(res->body);
    if (!body.contains("error")) {
        throw std::runtime_error("wp27 expected error (body=" + body.dump() + ")");
    }
    if (body["error"]["code"].get<int>() != -32602) {
        throw std::runtime_error("wp27 bad code");
    }
    const std::string msg = body["error"]["message"].get<std::string>();
    if (msg.find("input_policy_violation") == std::string::npos) {
        throw std::runtime_error("wp27 bad message");
    }
    if (!body["error"].contains("data") || !body["error"]["data"].contains("violations")) {
        throw std::runtime_error("wp27 missing violations");
    }
}

} // namespace

int main() {
    // Ensure relaxed override from the parent shell cannot affect this process (strict WP2.7 checks).
    (void)::unsetenv("AGENT_INPUT_STRICT");
    (void)::setenv("AGENT_INPUT_STRICT", "1", 1);
    (void)::setenv("AGENT_SERVER_BIND", "127.0.0.1", 1);
    (void)::setenv("AGENT_SERVER_JSON_RPC_PATH", "/rpc", 1);
    (void)::setenv("AGENT_SERVER_LEGACY_REST", "0", 1);
    (void)::setenv("AGENT_A2A_STRICT", "1", 1);
    (void)::setenv("AGENT_SERVER_SSE_PING_SEC", "0", 1);
    (void)::setenv("AGENT_SERVER_WORKER_THREADS", "2", 1);
    (void)::setenv("AGENT_SERVER_EXECUTOR_THREADS", "1", 1);

    agent_framework::AgentServer server(0);
    agent_framework::AgentCard card;
    card.name = "wp27";
    card.description = "input policy";
    card.provider = "test";
    card.api_endpoint = "http://127.0.0.1:9/rpc";
    server.register_agent_card(card);

    agent_framework::test::configure_execution_profile(server);

    std::thread th([&] { server.start(); });
    if (!wait_bound(server, 5000)) {
        server.stop();
        th.join();
        std::cerr << "test_agent_server_wp27_input: bind timeout\n";
        return 1;
    }
    const int port = server.bound_port();
    card.api_endpoint = "http://127.0.0.1:" + std::to_string(port) + "/rpc";
    server.register_agent_card(card);

    try {
        send_bad_cmd_expect_32602(port);
    } catch (const std::exception& e) {
        server.stop();
        th.join();
        std::cerr << "test_agent_server_wp27_input: " << e.what() << '\n';
        return 1;
    }

    server.stop();
    th.join();
    std::cout << "test_agent_server_wp27_input: ok\n";
    return 0;
}
