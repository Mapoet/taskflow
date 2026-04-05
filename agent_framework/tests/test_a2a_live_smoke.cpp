/**
 * @file test_a2a_live_smoke.cpp
 * @brief Tier C: AGENT_A2A_LIVE_TEST + AGENT_A2A_LIVE_BASE_URL (http origin); negative JSON-RPC codes
 */
#include <agent/agent_client.hpp>
#include <agent/a2a/client_config.hpp>
#include <agent/a2a/jsonrpc.hpp>
#include <agent/types.hpp>

#include <chrono>
#include <cstdlib>
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
using agent_framework::AgentClient;
using agent_framework::AgentClientOptions;
using agent_framework::AgentMessage;
using agent_framework::AgentPart;
using agent_framework::AgentTask;

void fail(const char* m) {
    std::cerr << "test_a2a_live_smoke: " << m << "\n";
    std::exit(1);
}

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

bool parse_host_port(const std::string& origin, std::string& host, int& port) {
    if (origin.rfind("http://", 0) != 0) {
        return false;
    }
    std::string u = origin.substr(7);
    const std::size_t slash = u.find('/');
    if (slash != std::string::npos) {
        u = u.substr(0, slash);
    }
    const std::size_t colon = u.rfind(':');
    if (colon == std::string::npos) {
        host = u;
        port = 80;
        return true;
    }
    host = u.substr(0, colon);
    port = std::atoi(u.substr(colon + 1).c_str());
    return port > 0 && port <= 65535;
}

void rpc_post_raw(const std::string& origin,
                  const std::string& rpc_path,
                  const std::string& body,
                  int expect_jsonrpc_code) {
    std::string host;
    int port = 0;
    if (!parse_host_port(origin, host, port)) {
        fail("rpc_post_raw: need http://host:port origin");
    }
    httplib::Client cli(host.c_str(), port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(5, 0);
    auto res = cli.Post(rpc_path.c_str(), body, "application/json");
    if (!res) {
        fail("rpc_post_raw: HTTP failed");
    }
    if (res->status != 200) {
        fail("rpc_post_raw: HTTP status not 200");
    }
    json j = json::parse(res->body);
    auto c = agent_framework::a2a::try_get_jsonrpc_error_code(j);
    if (!c.has_value() || *c != expect_jsonrpc_code) {
        fail("rpc_post_raw: unexpected error code");
    }
}

} // namespace

int main() {
    const char* live = std::getenv("AGENT_A2A_LIVE_TEST");
    const char* base_env = std::getenv("AGENT_A2A_LIVE_BASE_URL");
    if (live == nullptr || live[0] == '\0' || live[0] == '0') {
        std::cout << "test_a2a_live_smoke: SKIP (set AGENT_A2A_LIVE_TEST=1 and AGENT_A2A_LIVE_BASE_URL)\n";
        return 0;
    }
    if (base_env == nullptr || base_env[0] == '\0') {
        std::cout << "test_a2a_live_smoke: SKIP (AGENT_A2A_LIVE_BASE_URL unset)\n";
        return 0;
    }
    std::string origin = base_env;
    while (!origin.empty() && origin.back() == '/') {
        origin.pop_back();
    }
    if (origin.rfind("http://", 0) != 0) {
        std::cout << "test_a2a_live_smoke: SKIP (only http:// origins supported for raw negative tests)\n";
        return 0;
    }

    try {
        AgentClientOptions disc_opts;
        disc_opts.use_legacy_rest = false;
        disc_opts.json_rpc_path = "/rpc";
        AgentClient discover_cli(origin, disc_opts);

        AgentCard card = discover_cli.discover_agent("/.well-known/agent-card.json").get();
        std::string rpc_base;
        std::string rpc_path;
        split_api_endpoint(card.api_endpoint, rpc_base, rpc_path);

        AgentClientOptions rpc_opts;
        rpc_opts.use_legacy_rest = false;
        rpc_opts.json_rpc_path = rpc_path;
        AgentClient rpc_cli(rpc_base, rpc_opts);

        const char* tok = std::getenv("AGENT_A2A_LIVE_TOKEN");
        if (tok != nullptr && tok[0] != '\0') {
            json auth;
            auth["type"] = "bearer";
            auth["token"] = std::string(tok);
            rpc_cli.set_authentication(auth);
            discover_cli.set_authentication(auth);
        }

        AgentMessage msg;
        msg.role = AgentMessage::Role::USER;
        AgentPart p;
        p.type = AgentPart::Type::TEXT;
        p.text = std::string("live smoke");
        msg.parts.push_back(std::move(p));
        AgentTask t = rpc_cli.send_task("", msg, std::nullopt, json::object()).get();
        if (t.task_id.empty()) {
            fail("empty task id");
        }
        for (int i = 0; i < 200; ++i) {
            t = rpc_cli.get_task("", t.task_id).get();
            if (t.status == agent_framework::AgentTaskStatus::COMPLETED ||
                t.status == agent_framework::AgentTaskStatus::FAILED ||
                t.status == agent_framework::AgentTaskStatus::CANCELLED) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        rpc_post_raw(origin, rpc_path, "{not-json", agent_framework::a2a::JsonRpcErrorCode::parse_error);

        json bad_method = {{"jsonrpc", "2.0"}, {"method", "NoSuchMethod"}, {"id", 9}, {"params", json::object()}};
        rpc_post_raw(origin, rpc_path, bad_method.dump(),
                     agent_framework::a2a::JsonRpcErrorCode::method_not_found);

        json bad_params = {{"jsonrpc", "2.0"},
                           {"method", agent_framework::a2a::kMethodSendMessage},
                           {"id", 10},
                           {"params", json::object()}};
        rpc_post_raw(origin, rpc_path, bad_params.dump(),
                     agent_framework::a2a::JsonRpcErrorCode::invalid_params);
    } catch (const std::exception& e) {
        std::cerr << "test_a2a_live_smoke: " << e.what() << "\n";
        return 1;
    }

    std::cout << "test_a2a_live_smoke: ok\n";
    return 0;
}
