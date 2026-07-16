/**
 * @file test_agent_client_auth_query.cpp
 * @brief AgentClient api_key_query appended only on GET discover (WP2.5 I-4)
 */
#include <agent/agent_client/agent_client.hpp>
#include <agent/a2a/wire_card.hpp>

#include <httplib.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using agent_framework::AgentClient;
using agent_framework::AgentCard;
using agent_framework::a2a::agent_card_to_a2a_wire;
using agent_framework::json;

static std::string last_query_key;

static void fail(const char* m) {
    std::cerr << "test_agent_client_auth_query: " << m << "\n";
    std::exit(1);
}

int main() {
    httplib::Server svr;
    svr.Get("/.well-known/agent-card.json", [](const httplib::Request& req, httplib::Response& res) {
        last_query_key = req.get_param_value("api_key");
        AgentCard c;
        c.name = "q";
        c.description = "d";
        c.provider = "p";
        c.api_endpoint = "http://127.0.0.1:9/rpc";
        res.status = 200;
        res.set_content(agent_card_to_a2a_wire(c).dump(), "application/json");
    });

    std::thread th([&] { svr.listen("127.0.0.1", 18099); });
    for (int i = 0; i < 200; ++i) {
        if (svr.is_running()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!svr.is_running()) {
        fail("server not up");
    }

    AgentClient cli("http://127.0.0.1:18099", {});
    json q;
    q["type"] = "api_key_query";
    q["key_value"] = "secret123";
    q["param_name"] = "api_key";
    cli.set_authentication(q);

    auto fut = cli.discover_agent("/.well-known/agent-card.json");
    AgentCard card = fut.get();
    (void)card;

    if (last_query_key != "secret123") {
        svr.stop();
        th.join();
        fail("expected api_key query on GET discover");
    }

    svr.stop();
    th.join();
    std::cout << "test_agent_client_auth_query: ok\n";
    return 0;
}
