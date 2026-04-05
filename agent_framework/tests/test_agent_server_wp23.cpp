/**
 * @file test_agent_server_wp23.cpp
 * @brief WP2.3 integration: I-1 cancel, I-2 timeout, I-3 PENDING cancel
 */
#include <agent/agent_server.hpp>
#include <agent/a2a/wire_mapping.hpp>
#include <agent/task_state_machine.hpp>
#include <agent/types.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
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
using agent_framework::AgentServer;
using agent_framework::AgentTask;
using agent_framework::AgentTaskStatus;
using agent_framework::AgentPart;
using agent_framework::TaskControl;
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

std::string jsonrpc_cancel(int port, const std::string& id) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(5, 0);
    json req;
    req["jsonrpc"] = "2.0";
    req["method"] = "CancelTask";
    req["id"] = 9;
    req["params"] = json::object({{"id", id}});
    auto res = cli.Post("/rpc", req.dump(), "application/json");
    if (!res || res->status != 200) {
        throw std::runtime_error("cancel post");
    }
    json body = json::parse(res->body);
    if (body.contains("error")) {
        throw std::runtime_error("cancel rpc error");
    }
    return body["result"]["id"].get<std::string>();
}

std::string jsonrpc_send(int port, const std::string& text, const json& metadata = json::object()) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(5, 0);
    json msg;
    msg["messageId"] = "m";
    msg["role"] = "ROLE_USER";
    msg["parts"] = json::array({json{{"text", text}, {"mediaType", "text/plain"}}});
    json params;
    params["message"] = msg;
    params["metadata"] = metadata;
    json req;
    req["jsonrpc"] = "2.0";
    req["method"] = "SendMessage";
    req["id"] = 1;
    req["params"] = params;
    auto res = cli.Post("/rpc", req.dump(), "application/json");
    if (!res || res->status != 200) {
        throw std::runtime_error("send post");
    }
    json body = json::parse(res->body);
    if (body.contains("error")) {
        throw std::runtime_error("send rpc error");
    }
    return body["result"]["task"]["id"].get<std::string>();
}

json jsonrpc_get_task(int port, const std::string& id) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(5, 0);
    json req;
    req["jsonrpc"] = "2.0";
    req["method"] = "GetTask";
    req["id"] = 2;
    req["params"] = json::object({{"id", id}});
    auto res = cli.Post("/rpc", req.dump(), "application/json");
    if (!res || res->status != 200) {
        throw std::runtime_error("gettask post");
    }
    json body = json::parse(res->body);
    if (body.contains("error")) {
        throw std::runtime_error("gettask rpc error");
    }
    return body["result"];
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
        ::setenv("AGENT_SERVER_WORKER_THREADS", "1", 1);
        ::setenv("AGENT_SERVER_EXECUTOR_THREADS", "2", 1);
        ::unsetenv("AGENT_TASK_DEFAULT_TIMEOUT_SEC");

        AgentCard card;
        card.name = "wp23";
        card.description = "test";
        card.provider = "local";
        card.api_endpoint = "http://127.0.0.1:9/rpc";
        server.register_agent_card(card);

        std::map<std::string, int> runs;
        std::mutex run_mu;

        server.set_task_handler([&runs, &run_mu](AgentTask t,
                                                 std::shared_ptr<workflow::GraphBuilder>,
                                                 std::shared_ptr<TaskControl> control) {
            return std::async(std::launch::async, [t, control, &runs, &run_mu]() mutable {
                bool slow = false;
                bool coop_timeout = false;
                for (const auto& m : t.messages) {
                    for (const auto& p : m.parts) {
                        if (p.type == AgentPart::Type::TEXT && p.text) {
                            if (*p.text == "slow_cancel") {
                                slow = true;
                            }
                            if (*p.text == "timeout_coop") {
                                slow = true;
                                coop_timeout = true;
                            }
                        }
                    }
                }
                {
                    std::lock_guard<std::mutex> lk(run_mu);
                    runs[t.task_id] += 1;
                }
                if (!slow) {
                    t.status = AgentTaskStatus::COMPLETED;
                    t.updated_at = std::chrono::system_clock::now();
                    return t;
                }
                for (int iter = 0;; ++iter) {
                    if (control) {
                        control->check_deadline_now();
                        if (coop_timeout && control->is_deadline_exceeded()) {
                            t.status = AgentTaskStatus::FAILED;
                            t.metadata["a2a_failure_reason"] = "timeout";
                            t.updated_at = std::chrono::system_clock::now();
                            return t;
                        }
                        if (control->is_cancel_requested()) {
                            t.status = AgentTaskStatus::CANCELLED;
                            t.updated_at = std::chrono::system_clock::now();
                            return t;
                        }
                    }
                    if (iter >= 24) {
                        t.status = AgentTaskStatus::COMPLETED;
                        t.updated_at = std::chrono::system_clock::now();
                        return t;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            });
        });

        th = std::thread([&] { server.start(); });
        if (!wait_bound(server, 5000)) {
            server.stop();
            th.join();
            throw std::runtime_error("bound port");
        }
        const int port = server.bound_port();

        // I-3: third task cancelled while queued; handler must not run
        std::string id1 = jsonrpc_send(port, "slow_cancel");
        std::string id2 = jsonrpc_send(port, "slow_cancel");
        std::string id3 = jsonrpc_send(port, "slow_cancel");
        jsonrpc_cancel(port, id3);
        std::this_thread::sleep_for(std::chrono::milliseconds(3200));
        {
            std::lock_guard<std::mutex> lk(run_mu);
            if (runs[id3] != 0) {
                throw std::runtime_error("I-3 handler ran for cancelled pending task");
            }
            if (runs[id1] < 1 || runs[id2] < 1) {
                throw std::runtime_error("I-3 expected first two tasks to run");
            }
        }
        json gt3 = jsonrpc_get_task(port, id3);
        if (gt3["status"]["state"].get<std::string>() != "TASK_STATE_CANCELED") {
            throw std::runtime_error("I-3 task3 not canceled on wire");
        }

        // I-2
        json meta;
        meta["timeout_sec"] = 1;
        std::string idt = jsonrpc_send(port, "timeout_coop", meta);
        std::this_thread::sleep_for(std::chrono::milliseconds(3200));
        json gtt = jsonrpc_get_task(port, idt);
        if (gtt["status"]["state"].get<std::string>() != "TASK_STATE_FAILED") {
            throw std::runtime_error("I-2 not FAILED");
        }

        // I-1: cancel during WORKING; observe via GetTask (and SSE substring if available)
        std::string idc = jsonrpc_send(port, "slow_cancel");
        std::atomic<bool> cancel_seen{false};
        std::thread sse_th([port, idc, &cancel_seen] {
            httplib::Client c("127.0.0.1", port);
            c.set_read_timeout(10, 0);
            std::string path = std::string("/tasks/sendSubscribe?task_id=") + idc;
            std::string acc;
            c.Get(path.c_str(), [&](const char* data, std::size_t len) {
                acc.append(data, len);
                if (acc.find("TASK_STATE_CANCELED") != std::string::npos) {
                    cancel_seen.store(true);
                    return false;
                }
                return true;
            });
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        jsonrpc_cancel(port, idc);
        for (int i = 0; i < 50; ++i) {
            json g = jsonrpc_get_task(port, idc);
            if (g["status"]["state"].get<std::string>() == "TASK_STATE_CANCELED") {
                cancel_seen.store(true);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        sse_th.join();

        if (!cancel_seen.load()) {
            throw std::runtime_error("I-1 cancel not observed");
        }

        server.stop();
        th.join();

        std::cout << "test_agent_server_wp23: ok\n";
        return 0;
    } catch (const std::exception& e) {
        server.stop();
        if (th.joinable()) {
            th.join();
        }
        std::cerr << "test_agent_server_wp23: fail: " << e.what() << '\n';
        return 1;
    }
}
