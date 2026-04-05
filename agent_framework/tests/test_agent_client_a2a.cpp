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

int pick_listen_port() {
    httplib::Server s;
    s.Get("/p", [](const httplib::Request&, httplib::Response& res) { res.set_content("ok", "text/plain"); });
    const int p = s.bind_to_any_port("127.0.0.1");
    if (p <= 0) {
        fail("bind_to_any_port");
    }
    return p;
}

/** C-1: JSON-RPC SendMessage → task id */
void test_c1_jsonrpc_send() {
    const int port = pick_listen_port();
    httplib::Server srv;
    srv.Post("/", [](const httplib::Request& req, httplib::Response& res) {
        json body = json::parse(req.body);
        if (body.value("method", std::string()) != "SendMessage") {
            res.status = 400;
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

    std::thread th([&] { srv.listen("127.0.0.1", port); });
    while (!srv.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
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

    AgentTask t = cli.send_task("", msg, std::nullopt, json::object()).get();
    if (t.task_id != "task-c1") {
        srv.stop();
        th.join();
        fail("C-1 task_id");
    }
    srv.stop();
    th.join();
}

/** C-2: JSON-RPC error → A2aRpcException */
void test_c2_jsonrpc_error() {
    const int port = pick_listen_port();
    httplib::Server srv;
    srv.Post("/", [](const httplib::Request& req, httplib::Response& res) {
        json body = json::parse(req.body);
        json out = {{"jsonrpc", "2.0"},
                    {"id", body["id"]},
                    {"error", {{"code", -32602}, {"message", "invalid"}}}};
        res.set_content(out.dump(), "application/json");
    });

    std::thread th([&] { srv.listen("127.0.0.1", port); });
    while (!srv.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    AgentClientOptions a2a_opts;
    a2a_opts.use_legacy_rest = false;
    AgentClient cli("http://127.0.0.1:" + std::to_string(port), a2a_opts);
    AgentMessage msg;
    msg.role = AgentMessage::Role::USER;
    AgentPart p;
    p.type = AgentPart::Type::TEXT;
    p.text = std::string("x");
    msg.parts.push_back(std::move(p));

    try {
        (void)cli.send_task("", msg, std::nullopt, json::object()).get();
        srv.stop();
        th.join();
        fail("C-2 expected exception");
    } catch (const A2aRpcException& e) {
        if (e.code() != -32602) {
            srv.stop();
            th.join();
            fail("C-2 code");
        }
    } catch (...) {
        srv.stop();
        th.join();
        fail("C-2 wrong exception type");
    }
    srv.stop();
    th.join();
}

/** C-3: Legacy REST path（实例级 `use_legacy_rest`） */
void test_c3_legacy_rest() {
    const int port = pick_listen_port();
    std::string last_path;
    httplib::Server srv;
    srv.Post("/prefix/tasks/send", [&](const httplib::Request& req, httplib::Response& res) {
        last_path = req.path;
        json task_body = {{"task_id", "leg1"}, {"status", "pending"}, {"metadata", json::object()}};
        res.set_content(json{{"task", task_body}}.dump(), "application/json");
    });

    std::thread th([&] { srv.listen("127.0.0.1", port); });
    while (!srv.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    AgentClientOptions leg_opts;
    leg_opts.use_legacy_rest = true;
    AgentClient cli("http://127.0.0.1:" + std::to_string(port), leg_opts);
    AgentMessage msg;
    msg.role = AgentMessage::Role::USER;
    AgentPart p;
    p.type = AgentPart::Type::TEXT;
    p.text = std::string("x");
    msg.parts.push_back(std::move(p));

    (void)cli.send_task("/prefix", msg, std::nullopt, json::object()).get();
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
    const int port = pick_listen_port();
    httplib::Server srv;

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

    std::thread th([&] { srv.listen("127.0.0.1", port); });
    while (!srv.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    agent_framework::HttplibClient http;
    http.set_timeout_sec(30);
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
        5,
        &cancel);

    if (status_count.load() < 2) {
        srv.stop();
        th.join();
        fail("C-4 status frames");
    }

    srv.stop();
    th.join();
}

} // namespace

int main() {
    try {
        test_c1_jsonrpc_send();
        test_c2_jsonrpc_error();
        test_c3_legacy_rest();
        test_c4_get_sse();
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    return 0;
}
