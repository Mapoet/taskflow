/**
 * @file test_agent_server_wp22.cpp
 * @brief AgentServer WP2.2：健康检查、JSON-RPC、异步 handler、SSE（S-1–S-4，E-1–E-2）
 */
#include <agent/agent_server.hpp>
#include <agent/a2a/sse_framing.hpp>
#include <agent/a2a/wire_mapping.hpp>
#include <agent/types.hpp>

#include <chrono>
#include <cstdlib>
#include <future>
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

using agent_framework::AgentCard;
using agent_framework::AgentServer;
using agent_framework::AgentTask;
using agent_framework::AgentTaskStatus;
using agent_framework::AgentPart;
using agent_framework::a2a::SseParser;
using agent_framework::a2a::try_parse_task_status_sse;
using json = nlohmann::json;

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

void s1_health(int port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(2, 0);
    auto res = cli.Get("/health");
    if (!res || res->status != 200) {
        throw std::runtime_error("S-1 health");
    }
    json j = json::parse(res->body);
    if (!j.value("ok", false)) {
        throw std::runtime_error("S-1 ok");
    }
}

std::string s2_jsonrpc_send(int port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(5, 0);
    json req = json::parse(R"({
        "jsonrpc": "2.0",
        "method": "SendMessage",
        "id": 42,
        "params": {
            "message": {
                "messageId": "m-s2",
                "role": "ROLE_USER",
                "parts": [{"text": "hi", "mediaType": "text/plain"}]
            }
        }
    })");
    auto res = cli.Post("/rpc", req.dump(), "application/json");
    if (!res || res->status != 200) {
        throw std::runtime_error("S-2 post");
    }
    json body = json::parse(res->body);
    if (body.contains("error")) {
        throw std::runtime_error("S-2 rpc error");
    }
    if (!body.contains("result") || !body["result"].contains("task")) {
        throw std::runtime_error("S-2 result.task");
    }
    return body["result"]["task"]["id"].get<std::string>();
}

void s3_async_timing(int port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(5, 0);
    json req = json::parse(R"({
        "jsonrpc": "2.0",
        "method": "SendMessage",
        "id": 1,
        "params": {
            "message": {
                "messageId": "m-s3",
                "role": "ROLE_USER",
                "parts": [{"text": "slow", "mediaType": "text/plain"}]
            }
        }
    })");
    const auto t0 = std::chrono::steady_clock::now();
    auto res = cli.Post("/rpc", req.dump(), "application/json");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    if (!res || res->status != 200) {
        throw std::runtime_error("S-3 post");
    }
    json body = json::parse(res->body);
    if (body.contains("error")) {
        throw std::runtime_error("S-3 error");
    }
    if (ms >= 500) {
        throw std::runtime_error("S-3 not async enough");
    }
}

void s4_concurrent_posts(int port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(10, 0);
    auto send_one = [&](int k) {
        json req;
        req["jsonrpc"] = "2.0";
        req["method"] = "SendMessage";
        req["id"] = k;
        req["params"]["message"] = json::parse(R"({
            "messageId": "m-c",
            "role": "ROLE_USER",
            "parts": [{"text": "x", "mediaType": "text/plain"}]
        })");
        req["params"]["message"]["messageId"] = "m-c-" + std::to_string(k);
        auto res = cli.Post("/rpc", req.dump(), "application/json");
        if (!res || res->status != 200) {
            throw std::runtime_error("S-4 post");
        }
        json body = json::parse(res->body);
        if (body.contains("error")) {
            throw std::runtime_error("S-4 err");
        }
        return body["result"]["task"]["id"].get<std::string>();
    };
    auto a = std::async(std::launch::async, [&] { return send_one(10); });
    auto b = std::async(std::launch::async, [&] { return send_one(11); });
    std::string ida = a.get();
    std::string idb = b.get();
    if (ida == idb) {
        throw std::runtime_error("S-4 ids");
    }
}

void e1_e2_sse(int port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(20, 0);

    json req = json::parse(R"({
        "jsonrpc": "2.0",
        "method": "SendMessage",
        "id": 99,
        "params": {
            "message": {
                "messageId": "m-sse",
                "role": "ROLE_USER",
                "parts": [{"text": "sse", "mediaType": "text/plain"}]
            }
        }
    })");
    auto pres = cli.Post("/rpc", req.dump(), "application/json");
    if (!pres || pres->status != 200) {
        throw std::runtime_error("E pres");
    }
    json body = json::parse(pres->body);
    const std::string tid = body["result"]["task"]["id"].get<std::string>();

    SseParser parser;
    bool got_first = false;
    bool got_completed = false;
    const std::string path = "/tasks/sendSubscribe?task_id=" + tid;

    auto sres = cli.Get(path.c_str(), [&](const char* data, std::size_t len) {
        parser.feed(std::string_view(data, len));
        std::vector<agent_framework::a2a::SseEvent> evs;
        parser.drain_events(evs);
        for (const auto& ev : evs) {
            AgentTask parsed;
            if (!try_parse_task_status_sse(ev, parsed)) {
                continue;
            }
            if (parsed.task_id != tid) {
                return false;
            }
            if (!got_first) {
                got_first = true;
            }
            if (parsed.status == AgentTaskStatus::COMPLETED) {
                got_completed = true;
                return false;
            }
        }
        return true;
    });

    // Chunked SSE: receiver may stop with Error::Canceled; EOF may appear as Error::Read; Result may be null.
    const bool transport_ok = static_cast<bool>(sres) ||
                              sres.error() == httplib::Error::Read ||
                              sres.error() == httplib::Error::Canceled;
    if (!transport_ok) {
        throw std::runtime_error("E sse connect");
    }
    if (static_cast<bool>(sres) && sres->status != 200) {
        throw std::runtime_error("E sse status");
    }
    if (!got_first) {
        throw std::runtime_error("E-1 no sse");
    }
    if (!got_completed) {
        throw std::runtime_error("E-2 completed");
    }
}

} // namespace

int main() {
    AgentServer server(0);
    std::thread th;
    try {
        ::setenv("AGENT_SERVER_BIND", "127.0.0.1", 1);
        ::setenv("AGENT_SERVER_JSON_RPC_PATH", "/rpc", 1);
        ::setenv("AGENT_SERVER_LEGACY_REST", "0", 1);
        ::setenv("AGENT_A2A_STRICT", "1", 1);
        ::setenv("AGENT_SERVER_SSE_PING_SEC", "0", 1);
        ::setenv("AGENT_SERVER_WORKER_THREADS", "4", 1);
        ::setenv("AGENT_SERVER_EXECUTOR_THREADS", "2", 1);

        AgentCard card;
        card.name = "test";
        card.description = "wp22";
        card.provider = "local";
        card.api_endpoint = "http://127.0.0.1:9/rpc";
        server.register_agent_card(card);

        server.set_task_handler([](AgentTask t, std::shared_ptr<workflow::GraphBuilder>) {
            return std::async(std::launch::async, [t]() mutable {
                for (const auto& m : t.messages) {
                    for (const auto& p : m.parts) {
                        if (p.type == AgentPart::Type::TEXT && p.text.has_value()) {
                            if (*p.text == "slow") {
                                std::this_thread::sleep_for(std::chrono::seconds(2));
                            } else if (*p.text == "sse") {
                                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                            }
                        }
                    }
                }
                t.status = AgentTaskStatus::COMPLETED;
                t.updated_at = std::chrono::system_clock::now();
                return t;
            });
        });

        th = std::thread([&] { server.start(); });
        if (!wait_bound(server, 5000)) {
            server.stop();
            th.join();
            throw std::runtime_error("bound port");
        }
        const int port = server.bound_port();

        s1_health(port);
        s2_jsonrpc_send(port);
        s4_concurrent_posts(port);
        e1_e2_sse(port);
        s3_async_timing(port);

        server.stop();
        th.join();
        std::cout << "test_agent_server_wp22: ok\n";
        return 0;
    } catch (const std::exception& e) {
        server.stop();
        if (th.joinable()) {
            th.join();
        }
        std::cerr << "test_agent_server_wp22: fail: " << e.what() << '\n';
        return 1;
    }
}
