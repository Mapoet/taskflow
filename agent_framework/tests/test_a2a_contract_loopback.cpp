/**
 * @file test_a2a_contract_loopback.cpp
 * @brief WP2.6: fixture replay loopback — mock JSON-RPC returns golden response; optional Bearer (L-2)
 */
#include <agent/agent_client/agent_client.hpp>
#include <agent/a2a/client_config.hpp>
#include <agent/core/types.hpp>

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#if __has_include(<httplib/httplib.hpp>)
#include <httplib/httplib.hpp>
#elif __has_include(<httplib.hpp>)
#include <httplib.hpp>
#else
#include <httplib.h>
#endif

using agent_framework::AgentClient;
using agent_framework::AgentClientOptions;
using agent_framework::AgentMessage;
using agent_framework::AgentPart;
using agent_framework::AgentTask;
using agent_framework::json;
namespace a2a = agent_framework::a2a;

#ifndef AGENT_TEST_A2A_ROOT
#define AGENT_TEST_A2A_ROOT "."
#endif

namespace {

void fail(const char* m) {
    std::cerr << "test_a2a_contract_loopback: " << m << "\n";
    std::exit(1);
}

bool looks_like_httplib_transport_fail(const std::exception& e) {
    const char* w = e.what();
    return w != nullptr && std::strstr(w, "HttplibClient:") != nullptr
        && std::strstr(w, "failed, error code:") != nullptr;
}

/** cpp-httplib may return Error::Read / Connection right after listen; retry bounded (see test_agent_client_a2a). */
template <typename Fn>
void retry_httplib_transport_void(Fn&& fn, int max_attempts = 8) {
    for (int i = 0; i < max_attempts; ++i) {
        try {
            fn();
            return;
        } catch (const std::exception& e) {
            if (i + 1 == max_attempts || !looks_like_httplib_transport_fail(e)) {
                throw;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(80 * (i + 1)));
        }
    }
}

/** Ensures listen thread is never destroyed joinable (avoids std::terminate on exception paths). */
struct ListenThread {
    httplib::Server& srv;
    std::thread worker;
    // port must be captured by value: the ctor parameter does not outlive this ctor body.
    ListenThread(httplib::Server& s, int port) : srv(s), worker([&s, port] { s.listen("127.0.0.1", port); }) {}
    explicit ListenThread(httplib::Server& s) : srv(s), worker([&s] { s.listen_after_bind(); }) {}
    ~ListenThread() {
        try {
            srv.stop();
        } catch (...) {
            // destructor must not throw
        }
        if (worker.joinable()) {
            worker.join();
        }
    }
    ListenThread(const ListenThread&) = delete;
    ListenThread& operator=(const ListenThread&) = delete;
};

json read_fixture_json(const char* rel) {
    std::string path = std::string(AGENT_TEST_A2A_ROOT) + "/synthetic-v1/" + rel;
    std::ifstream in(path);
    if (!in) {
        std::ostringstream oss;
        oss << "missing fixture: " << path;
        fail(oss.str().c_str());
    }
    json j;
    in >> j;
    return j;
}

void wait_until_running(httplib::Server& srv) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!srv.is_running()) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(5)) {
            fail("server is_running timeout");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void wait_health_ok(const std::string& host, int port, int timeout_ms) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        httplib::Client c(host, port);
        c.set_connection_timeout(0, 300000); // 300ms
        c.set_read_timeout(1, 0);
        c.set_write_timeout(1, 0);
        auto r = c.Get("/health");
        if (r && r->status == 200) {
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
            return;
        }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeout_ms)) {
            fail("health timeout");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

AgentMessage make_fixture_user_message() {
    AgentMessage msg;
    msg.role = AgentMessage::Role::USER;
    AgentPart part;
    part.type = AgentPart::Type::TEXT;
    part.text = std::string("fixture hello");
    msg.parts.push_back(std::move(part));
    return msg;
}

void test_l1_fixture_send_message() {
    json golden_res = read_fixture_json("jsonrpc/send_message_response.json");

    httplib::Server srv;
    srv.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("OK", "text/plain");
    });
    srv.Post("/rpc", [golden_res](const httplib::Request& req, httplib::Response& res) {
        json req_j;
        try {
            req_j = json::parse(req.body);
        } catch (const json::exception&) {
            res.status = 400;
            return;
        }
        if (!req_j.contains("method") || req_j["method"] != a2a::kMethodSendMessage) {
            res.status = 400;
            return;
        }
        json out = golden_res;
        if (req_j.contains("id")) {
            out["id"] = req_j["id"];
        }
        res.status = 200;
        res.set_content(out.dump(), "application/json");
    });

    const int port = srv.bind_to_any_port("127.0.0.1");
    if (port <= 0) fail("bind_to_any_port L-1");
    ListenThread listen{srv};
    wait_until_running(srv);
    wait_health_ok("127.0.0.1", port, 15000);

    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    AgentClientOptions opt;
    opt.use_legacy_rest = false;
    opt.json_rpc_path = "/rpc";
    AgentClient cli(base, opt);

    AgentTask t = cli.send_task("/", make_fixture_user_message(), std::nullopt, json::object()).get();
    if (t.task_id != "task-syn-1") {
        fail("task_id mismatch vs fixture");
    }
}

void test_l2_bearer_optional() {
    const char* tok = std::getenv("AGENT_A2A_CONTRACT_TEST_BEARER");
    if (tok == nullptr || tok[0] == '\0') {
        std::cout << "test_a2a_contract_loopback: L-2 SKIP (AGENT_A2A_CONTRACT_TEST_BEARER unset)\n";
        return;
    }
    json golden_res = read_fixture_json("jsonrpc/send_message_response.json");
    httplib::Server srv;
    srv.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("OK", "text/plain");
    });
    srv.Post("/rpc", [golden_res, tok](const httplib::Request& req, httplib::Response& res) {
        std::string auth = req.get_header_value("Authorization");
        const std::string expect = std::string("Bearer ") + tok;
        if (auth != expect) {
            res.status = 401;
            res.set_content(R"({"error":"Unauthorized"})", "application/json");
            return;
        }
        json req_j;
        try {
            req_j = json::parse(req.body);
        } catch (const json::exception&) {
            res.status = 400;
            return;
        }
        json out = golden_res;
        if (req_j.contains("id")) {
            out["id"] = req_j["id"];
        }
        res.status = 200;
        res.set_content(out.dump(), "application/json");
    });

    const int port = srv.bind_to_any_port("127.0.0.1");
    if (port <= 0) fail("bind_to_any_port L-2");
    ListenThread listen{srv};
    wait_until_running(srv);
    wait_health_ok("127.0.0.1", port, 15000);

    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    AgentClientOptions opt;
    opt.use_legacy_rest = false;
    opt.json_rpc_path = "/rpc";

    for (int attempt = 0; attempt < 8; ++attempt) {
        try {
            AgentClient cli_bad(base, opt);
            cli_bad.send_task("/", make_fixture_user_message(), std::nullopt, json::object()).get();
            fail("expected failure without bearer");
        } catch (const std::exception& e) {
            if (looks_like_httplib_transport_fail(e) && attempt + 1 < 8) {
                std::this_thread::sleep_for(std::chrono::milliseconds(80 * (attempt + 1)));
                continue;
            }
            if (looks_like_httplib_transport_fail(e)) {
                throw;
            }
            break;
        }
    }

    AgentClient cli_ok(base, opt);
    json auth_cfg;
    auth_cfg["type"] = "bearer";
    auth_cfg["token"] = tok;
    cli_ok.set_authentication(auth_cfg);

    AgentTask t;
    retry_httplib_transport_void([&] {
        t = cli_ok.send_task("/", make_fixture_user_message(), std::nullopt, json::object()).get();
    });
    if (t.task_id != "task-syn-1") {
        fail("L-2 task_id mismatch");
    }
}

} // namespace

int main() {
    try {
        ::setenv("AGENT_CLIENT_USE_LEGACY_REST", "0", 1);
        test_l1_fixture_send_message();
        test_l2_bearer_optional();
    } catch (const std::exception& e) {
        std::cerr << "test_a2a_contract_loopback: exception: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "test_a2a_contract_loopback: non-standard exception\n";
        return 1;
    }
    std::cout << "test_a2a_contract_loopback: ok\n";
    return 0;
}
