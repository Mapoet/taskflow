/**
 * @file test_agent_client_a2a.cpp
 * @brief WP2.4 AgentClient JSON-RPC / Legacy / SSE (C-1–C-4)
 */
#include <agent/agent_client.hpp>
#include <agent/a2a/jsonrpc_client.hpp>
#include <agent/a2a/sse_framing.hpp>
#include <agent/a2a/wire_mapping.hpp>
#include <agent/httplib_http_client.hpp>
#include <agent/types.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#if __has_include(<httplib/httplib.hpp>)
#include <httplib/httplib.hpp>
#elif __has_include(<httplib.hpp>)
#include <httplib.hpp>
#else
#include <httplib.h>
#endif

namespace {

/** Under load, httplib listen may take >5s to accept; avoid flaky “server failed to become ready”. */
constexpr auto k_mock_server_ready_timeout = std::chrono::seconds(15);

using agent_framework::AgentClient;
using agent_framework::AgentClientOptions;
using agent_framework::AgentMessage;
using agent_framework::AgentPart;
using agent_framework::AgentTask;
using agent_framework::AgentTaskStatus;
using agent_framework::a2a::append_sse_event;
using agent_framework::a2a::A2aRpcException;
using agent_framework::a2a::stream_response_status_update;
using agent_framework::json;

void fail(const char* msg) {
    std::cerr << msg << "\n";
    std::exit(1);
}

/** After /health succeeds, the listen thread may still be ramping; avoids intermittent httplib Error::Read on POST. */
void settle_after_health_ready() {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

bool looks_like_httplib_transport_fail(const std::exception& e) {
    const char* w = e.what();
    return w != nullptr && std::strstr(w, "HttplibClient:") != nullptr
        && std::strstr(w, "failed, error code:") != nullptr;
}

/** cpp-httplib occasionally returns Error::Read on the first POST after listen; retry bounded. */
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

/** C-1: JSON-RPC SendMessage → task id */
void test_c1_jsonrpc_send() {
    httplib::Server srv;
    // Add health check endpoint for server readiness
    srv.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("OK", "text/plain");
    });
    srv.Post("/", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (const json::exception&) {
            res.status = 200;
            res.set_content(R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"parse"}})",
                            "application/json");
            return;
        }
        if (!body.contains("id")) {
            res.status = 200;
            res.set_content(
                R"({"jsonrpc":"2.0","id":null,"error":{"code":-32600,"message":"Invalid Request"}})",
                "application/json");
            return;
        }
        if (body.value("method", std::string()) != "SendMessage") {
            res.status = 200;
            res.set_content(
                json{{"jsonrpc", "2.0"},
                     {"id", body["id"]},
                     {"error", {{"code", -32601}, {"message", "Method not found"}}}}
                    .dump(),
                "application/json");
            return;
        }
        json task_wire = {
            {"id", "task-c1"},
            {"status",
             {{"state", "TASK_STATE_WORKING"}, {"timestamp", "2026-01-01T00:00:00Z"}}},
            {"metadata", json::object()}};
        json out = {{"jsonrpc", "2.0"}, {"id", body["id"]}, {"result", {{"task", task_wire}}}};
        res.set_content(out.dump(), "application/json");
    });

    const int port = srv.bind_to_any_port("127.0.0.1");
    if (port <= 0) fail("C-1 bind_to_any_port");
    std::thread th([&] { srv.listen_after_bind(); });
    while (!srv.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Wait for server to actually accept connections
    bool server_ready = false;
    const auto start = std::chrono::steady_clock::now();
    constexpr auto timeout = k_mock_server_ready_timeout;

    while (!server_ready && std::chrono::steady_clock::now() - start < timeout) {
        try {
            httplib::Client test_client("127.0.0.1", port);
            test_client.set_connection_timeout(2, 0);
            test_client.set_read_timeout(2, 0);
            test_client.set_write_timeout(2, 0);
            if (auto res = test_client.Get("/health")) {
                server_ready = true;
                std::cerr << "C-1: server health check passed" << std::endl;
                break;
            }
        } catch (...) {
            // Ignore and retry
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!server_ready) {
        srv.stop();
        th.join();
        fail("C-1 server failed to become ready");
    }

    AgentClientOptions a2a_opts;
    a2a_opts.use_legacy_rest = false;
    AgentClient cli("http://127.0.0.1:" + std::to_string(port), a2a_opts);
    AgentMessage msg;
    msg.role = AgentMessage::Role::USER;
    AgentPart p;
    p.type = AgentPart::Type::TEXT;
    p.text = std::string("hi");
    msg.parts.push_back(std::move(p));

    try {
        retry_httplib_transport_void([&] {
            AgentTask t = cli.send_task("", msg, std::nullopt, json::object()).get();
            if (t.task_id != "task-c1") {
                srv.stop();
                th.join();
                fail("C-1 task_id");
            }
        });
    } catch (...) {
        srv.stop();
        th.join();
        throw;
    }
    srv.stop();
    th.join();
}

/** C-2: JSON-RPC error → A2aRpcException */
void test_c2_jsonrpc_error() {
    httplib::Server srv;
    // Add health check endpoint for server readiness
    srv.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("OK", "text/plain");
    });
    srv.Post("/", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (const json::exception&) {
            res.status = 200;
            res.set_content(R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"parse"}})",
                            "application/json");
            return;
        }
        if (!body.contains("id")) {
            res.status = 200;
            res.set_content(
                R"({"jsonrpc":"2.0","id":null,"error":{"code":-32600,"message":"Invalid Request"}})",
                "application/json");
            return;
        }
        json out = {{"jsonrpc", "2.0"},
                    {"id", body["id"]},
                    {"error", {{"code", -32602}, {"message", "invalid"}}}};
        res.set_content(out.dump(), "application/json");
    });

    const int port = srv.bind_to_any_port("127.0.0.1");
    if (port <= 0) fail("C-2 bind_to_any_port");
    std::thread th([&] { srv.listen_after_bind(); });
    while (!srv.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Wait for server to actually accept connections
    bool server_ready = false;
    const auto start = std::chrono::steady_clock::now();
    constexpr auto timeout = k_mock_server_ready_timeout;

    while (!server_ready && std::chrono::steady_clock::now() - start < timeout) {
        try {
            httplib::Client test_client("127.0.0.1", port);
            test_client.set_connection_timeout(2, 0);
            test_client.set_read_timeout(2, 0);
            test_client.set_write_timeout(2, 0);
            if (auto res = test_client.Get("/health")) {
                server_ready = true;
                std::cerr << "C-2: server health check passed" << std::endl;
                break;
            }
        } catch (...) {
            // Ignore and retry
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!server_ready) {
        srv.stop();
        th.join();
        fail("C-2 server failed to become ready");
    }
    settle_after_health_ready();

    AgentClientOptions a2a_opts;
    a2a_opts.use_legacy_rest = false;
    AgentClient cli("http://127.0.0.1:" + std::to_string(port), a2a_opts);
    AgentMessage msg;
    msg.role = AgentMessage::Role::USER;
    AgentPart p;
    p.type = AgentPart::Type::TEXT;
    p.text = std::string("x");
    msg.parts.push_back(std::move(p));

    bool c2_ok = false;
    for (int attempt = 0; attempt < 8 && !c2_ok; ++attempt) {
        try {
            (void)cli.send_task("", msg, std::nullopt, json::object()).get();
            srv.stop();
            th.join();
            fail("C-2 expected exception");
        } catch (const A2aRpcException& e) {
            std::cerr << "C-2 caught A2aRpcException: code=" << e.code() << ", what=" << e.what() << std::endl;
            if (e.code() != -32602) {
                srv.stop();
                th.join();
                fail("C-2 code");
            }
            std::cerr << "C-2 test passed: correctly caught A2aRpcException with code -32602" << std::endl;
            c2_ok = true;
        } catch (const std::exception& e) {
            if (looks_like_httplib_transport_fail(e) && attempt + 1 < 8) {
                std::this_thread::sleep_for(std::chrono::milliseconds(80 * (attempt + 1)));
                continue;
            }
            std::cerr << "C-2 caught std::exception: " << e.what() << std::endl;
            srv.stop();
            th.join();
            fail("C-2 wrong exception type");
        }
    }
    if (!c2_ok) {
        srv.stop();
        th.join();
        fail("C-2 exhausted transport retries");
    }
    srv.stop();
    th.join();
}

/** C-3: Legacy REST path（实例级 `use_legacy_rest`） */
void test_c3_legacy_rest() {
    std::string last_path;
    httplib::Server srv;
    // Add health check endpoint for server readiness
    srv.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("OK", "text/plain");
    });
    srv.Post("/prefix/tasks/send", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            last_path = req.path;
            json task_body = {{"task_id", "leg1"}, {"status", "pending"}, {"metadata", json::object()}};
            res.set_content(json{{"task", task_body}}.dump(), "application/json");
        } catch (const std::exception&) {
            res.status = 500;
            res.set_content("{}", "application/json");
        }
    });

    const int port = srv.bind_to_any_port("127.0.0.1");
    if (port <= 0) fail("C-3 bind_to_any_port");
    std::thread th([&] { srv.listen_after_bind(); });
    while (!srv.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Wait for server to actually accept connections
    bool server_ready = false;
    const auto start = std::chrono::steady_clock::now();
    constexpr auto timeout = std::chrono::seconds(5);

    while (!server_ready && std::chrono::steady_clock::now() - start < timeout) {
        try {
            httplib::Client test_client("127.0.0.1", port);
            test_client.set_connection_timeout(1, 0);
            test_client.set_read_timeout(1, 0);
            test_client.set_write_timeout(1, 0);
            if (auto res = test_client.Get("/health")) {
                server_ready = true;
                std::cerr << "C-3: server health check passed" << std::endl;
                break;
            }
        } catch (...) {
            // Ignore and retry
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!server_ready) {
        srv.stop();
        th.join();
        fail("C-3 server failed to become ready");
    }
    settle_after_health_ready();

    AgentClientOptions leg_opts;
    leg_opts.use_legacy_rest = true;
    AgentClient cli("http://127.0.0.1:" + std::to_string(port), leg_opts);
    AgentMessage msg;
    msg.role = AgentMessage::Role::USER;
    AgentPart p;
    p.type = AgentPart::Type::TEXT;
    p.text = std::string("x");
    msg.parts.push_back(std::move(p));

    try {
        retry_httplib_transport_void([&] {
            (void)cli.send_task("/prefix", msg, std::nullopt, json::object()).get();
        });
    } catch (...) {
        srv.stop();
        th.join();
        throw;
    }
    if (last_path.find("/tasks/send") == std::string::npos) {
        srv.stop();
        th.join();
        fail("C-3 path");
    }
    srv.stop();
    th.join();
}

/** C-4: get_sse + StreamResponse ×2 */
void test_c4_get_sse() {
    httplib::Server srv;

    // Add health check endpoint for server readiness
    srv.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("OK", "text/plain");
    });

    srv.Get("/sse", [](const httplib::Request&, httplib::Response& res) {
        AgentTask t1;
        t1.task_id = "s1";
        t1.status = AgentTaskStatus::WORKING;
        std::string buf;
        append_sse_event(buf, "", stream_response_status_update(t1).dump());
        AgentTask t2;
        t2.task_id = "s1";
        t2.status = AgentTaskStatus::COMPLETED;
        append_sse_event(buf, "", stream_response_status_update(t2).dump());
        res.set_content(buf, "text/event-stream");
    });

    const int port = srv.bind_to_any_port("127.0.0.1");
    if (port <= 0) fail("C-4 bind_to_any_port");
    std::cerr << "C-4: starting test on port " << port << std::endl;
    std::thread th([&] { srv.listen_after_bind(); });
    while (!srv.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Wait for server to actually accept connections
    bool server_ready = false;
    const auto start = std::chrono::steady_clock::now();
    constexpr auto timeout = k_mock_server_ready_timeout;

    while (!server_ready && std::chrono::steady_clock::now() - start < timeout) {
        try {
            httplib::Client test_client("127.0.0.1", port);
            test_client.set_connection_timeout(2, 0);
            test_client.set_read_timeout(2, 0);
            test_client.set_write_timeout(2, 0);
            if (auto res = test_client.Get("/health")) {
                server_ready = true;
                std::cerr << "C-4: server health check passed" << std::endl;
                break;
            }
        } catch (...) {
            // Ignore and retry
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!server_ready) {
        srv.stop();
        th.join();
        fail("C-4 server failed to become ready");
    }
    settle_after_health_ready();

    agent_framework::HttplibClient http;
    http.set_timeout_sec(30);

    try {
        int parsed_status_frames = 0;
        for (int attempt = 0; attempt < 10 && parsed_status_frames < 2; ++attempt) {
            std::atomic<int> status_count{0};
            std::atomic<bool> cancel{false};
            agent_framework::a2a::SseParser parser;
            http.get_sse(
                "http://127.0.0.1:" + std::to_string(port) + "/sse",
                {},
                [&](std::string_view chunk) {
                    parser.feed(chunk);
                    std::vector<agent_framework::a2a::SseEvent> evs;
                    parser.drain_events(evs);
                    for (const auto& ev : evs) {
                        AgentTask tmp;
                        if (agent_framework::a2a::try_parse_task_status_sse(ev, tmp)) {
                            ++status_count;
                        }
                    }
                },
                30,
                &cancel);
            parsed_status_frames = status_count.load();
            if (parsed_status_frames < 2) {
                std::this_thread::sleep_for(std::chrono::milliseconds(80 * (attempt + 1)));
            }
        }
        if (parsed_status_frames < 2) {
            srv.stop();
            th.join();
            fail("C-4 status frames");
        }
    } catch (...) {
        srv.stop();
        th.join();
        throw;
    }
    srv.stop();
    th.join();
}

} // namespace

int main() {
    int passed = 0;
    int failed = 0;

    auto run_test = [&](auto test_func, const char* name) {
        std::cerr << "=== Running " << name << " ===" << std::endl;
        try {
            test_func();
            std::cerr << name << " PASSED" << std::endl;
            ++passed;
        } catch (const std::exception& e) {
            std::cerr << name << " FAILED: " << e.what() << std::endl;
            ++failed;
        }
    };

    run_test(test_c1_jsonrpc_send, "C-1 JSON-RPC SendMessage");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    run_test(test_c2_jsonrpc_error, "C-2 JSON-RPC error");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    run_test(test_c3_legacy_rest, "C-3 Legacy REST");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    run_test(test_c4_get_sse, "C-4 get_sse");

    std::cerr << "\n=== Summary ===" << std::endl;
    std::cerr << "Passed: " << passed << "/4" << std::endl;
    std::cerr << "Failed: " << failed << "/4" << std::endl;

    if (failed > 0) {
        return 1;
    }
    return 0;
}
