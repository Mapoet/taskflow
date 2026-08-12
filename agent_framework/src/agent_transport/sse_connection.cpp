/**
 * @file sse_connection.cpp
 * @brief SSE 客户端：get_sse + SseParser + StreamResponse（WP2.4）
 */
#include <agent/agent_transport/sse_connection.hpp>
#include <agent/agent_client/agent_client.hpp>
#include <agent/a2a/sse_framing.hpp>
#include <agent/a2a/wire_mapping.hpp>
#include <agent/agent_client/httplib_http_client.hpp>

#include <cstdlib>
#include <iostream>

namespace agent_framework {
namespace {

bool env_truthy(const char* v) {
    if (!v || !*v) {
        return false;
    }
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T';
}

bool sse_legacy_payload_enabled() {
    return env_truthy(std::getenv("AGENT_CLIENT_SSE_LEGACY_PAYLOAD"));
}

} // namespace

SSEConnection::SSEConnection(const std::string& endpoint, const std::string& task_id, HTTPClient* http,
                             std::optional<json> post_body)
    : endpoint_(endpoint), task_id_(task_id), http_client_(http), post_body_(std::move(post_body)) {}

SSEConnection::~SSEConnection() {
    close();
}

void SSEConnection::subscribe(const std::map<std::string, std::string>& headers,
                              std::function<void(const AgentTask&)> on_status_update,
                              std::function<void(const AgentArtifact&)> on_artifact_update,
                              std::function<void(const conversation::RuntimeEventEnvelope&)> on_runtime_event) {
    std::thread prev;
    {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (event_thread_.joinable()) {
            cancelled_.store(true, std::memory_order_release);
            prev = std::move(event_thread_);
        }
        on_status_update_ = std::move(on_status_update);
        on_artifact_update_ = std::move(on_artifact_update);
        on_runtime_event_ = std::move(on_runtime_event);
        request_headers_ = headers;

        cancelled_.store(false, std::memory_order_release);
        active_ = true;
        event_thread_ = std::thread(&SSEConnection::event_thread_func, this);
    }
    if (prev.joinable()) {
        prev.join();
    }
}

void SSEConnection::reconnect(const std::string& last_event_id) {
    std::map<std::string, std::string> headers;
    {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        headers = request_headers_;
    }
    reconnect(last_event_id, headers);
}

void SSEConnection::reconnect(const std::string& last_event_id,
                              const std::map<std::string, std::string>& headers) {
    std::thread prev;
    {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (event_thread_.joinable()) {
            cancelled_.store(true, std::memory_order_release);
            prev = std::move(event_thread_);
        }
        request_headers_ = headers;
        request_headers_["Last-Event-ID"] = last_event_id;
        cancelled_.store(false, std::memory_order_release);
        active_ = true;
        event_thread_ = std::thread(&SSEConnection::event_thread_func, this);
    }
    if (prev.joinable()) {
        prev.join();
    }
}

void SSEConnection::close() {
    std::thread prev;
    {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        cancelled_.store(true, std::memory_order_release);
        active_ = false;
        if (event_thread_.joinable()) {
            prev = std::move(event_thread_);
        }
    }
    if (prev.joinable()) {
        prev.join();
    }
}

bool SSEConnection::is_active() const {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    return active_;
}

std::uint64_t SSEConnection::runtime_cursor() const {
    return runtime_cursor_.load(std::memory_order_acquire);
}

void SSEConnection::handle_sse_event(const a2a::SseEvent& event) {
    try {
        AgentTask task;
        if (a2a::try_parse_task_status_sse(event, task)) {
            if (on_status_update_) {
                on_status_update_(task);
            }
            return;
        }
        if (a2a::try_parse_task_message_sse(event, task)) {
            if(on_status_update_) on_status_update_(task);
            return;
        }

        json root = json::parse(event.data);
        if (event.event == "runtime_event") {
            std::string error;
            auto runtime = conversation::decode_runtime_event(root, &error);
            if (!runtime || runtime->durability != conversation::EventDurability::Durable ||
                !event.id || *event.id != std::to_string(runtime->sequence))
                throw std::runtime_error(error.empty() ? "runtime_event_cursor_mismatch" : error);
            const auto prior = runtime_cursor_.load(std::memory_order_acquire);
            if (runtime->sequence <= prior) return;
            runtime_cursor_.store(runtime->sequence, std::memory_order_release);
            if (on_runtime_event_) on_runtime_event_(*runtime);
            return;
        }
        if (root.contains("artifactUpdate") && root["artifactUpdate"].is_object()) {
            const json& au = root["artifactUpdate"];
            if (au.contains("artifact") && on_artifact_update_) {
                AgentArtifact art = a2a::artifact_from_a2a_wire(au["artifact"]);
                on_artifact_update_(art);
            }
            return;
        }

        if (sse_legacy_payload_enabled() && root.contains("type")) {
            const std::string event_type = root.value("type", "");
            if (event_type == "task_status_update" && root.contains("task") && on_status_update_) {
                AgentTask t = AgentTask::from_json(root["task"]);
                on_status_update_(t);
            } else if (event_type == "artifact_update" && root.contains("artifact") && on_artifact_update_) {
                AgentArtifact a = AgentArtifact::from_json(root["artifact"]);
                on_artifact_update_(a);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "SSEConnection: parse error: " << e.what() << "\n";
    }
}

void SSEConnection::event_thread_func() {
    if (!http_client_) {
        return;
    }

    a2a::SseParser parser;

    try {
        if (post_body_) {
            auto* httplib = dynamic_cast<HttplibClient*>(http_client_);
            if (!httplib) {
                throw std::runtime_error("JSON-RPC SSE requires HttplibClient");
            }
            httplib->post_sse(
                endpoint_, *post_body_, request_headers_,
                [&](const std::string& event_name, const json& data, const std::string& event_id) {
                    a2a::SseEvent event;
                    event.event = event_name;
                    event.data = data.dump();
                    event.id = event_id;
                    handle_sse_event(event);
                }, 0);
            std::lock_guard<std::mutex> lock(connection_mutex_);
            active_ = false;
            return;
        }
        http_client_->get_sse(
            endpoint_,
            request_headers_,
            [&](std::string_view chunk) {
                parser.feed(chunk);
                std::vector<a2a::SseEvent> evs;
                parser.drain_events(evs);
                for (const auto& ev : evs) {
                    handle_sse_event(ev);
                }
            },
            0,
            &cancelled_);
    } catch (const std::exception& e) {
        // cpp-httplib reports a closed chunked response as a read/cancel error after
        // the terminal SSE frame. For JSON-RPC streams that is the normal lifecycle.
        if (!post_body_) {
            std::cerr << "SSEConnection: get_sse: " << e.what() << "\n";
        }
    }

    std::lock_guard<std::mutex> lock(connection_mutex_);
    active_ = false;
}

} // namespace agent_framework
