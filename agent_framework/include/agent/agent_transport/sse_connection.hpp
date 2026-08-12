/**
 * @file sse_connection.hpp
 * @brief SSE 连接管理器（A2A 协议，WP2.4 GET 流 + SseParser）
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_SSE_CONNECTION_H__
#define __AGENT_SSE_CONNECTION_H__

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <optional>
#include <agent/core/types.hpp>
#include <agent/conversation/types.hpp>
#include <agent/a2a/sse_framing.hpp>
#include <nlohmann/json.hpp>

namespace agent_framework {
using json = nlohmann::json;
}

namespace agent_framework {

class HTTPClient;

/**
 * @brief SSE 连接管理器：GET 长流 + WP2.1 StreamResponse（statusUpdate / artifactUpdate）
 */
class SSEConnection {
public:
    /**
     * @param endpoint Absolute SSE URL (http://…/tasks/sendSubscribe?task_id=…)
     * @param task_id Task id (for logging / legacy payload)
     * @param http Must outlive subscribe thread until close(); typically AgentClient's HttplibClient
     */
    SSEConnection(const std::string& endpoint, const std::string& task_id, HTTPClient* http,
                  std::optional<json> post_body = std::nullopt);

    ~SSEConnection();

    void subscribe(const std::map<std::string, std::string>& headers,
                   std::function<void(const AgentTask&)> on_status_update,
                   std::function<void(const AgentArtifact&)> on_artifact_update,
                   std::function<void(const conversation::RuntimeEventEnvelope&)> on_runtime_event = {});

    void reconnect(const std::string& last_event_id);
    /** Reconnect with freshly resolved authentication headers. */
    void reconnect(const std::string& last_event_id,
                   const std::map<std::string, std::string>& headers);

    void close();

    bool is_active() const;
    std::uint64_t runtime_cursor() const;

private:
    std::string endpoint_;
    std::string task_id_;
    HTTPClient* http_client_{nullptr};
    std::optional<json> post_body_;

    std::atomic<bool> cancelled_{false};
    bool active_ = false;
    std::thread event_thread_;
    mutable std::mutex connection_mutex_;

    std::map<std::string, std::string> request_headers_;

    std::function<void(const AgentTask&)> on_status_update_;
    std::function<void(const AgentArtifact&)> on_artifact_update_;
    std::function<void(const conversation::RuntimeEventEnvelope&)> on_runtime_event_;
    std::atomic<std::uint64_t> runtime_cursor_{0};

    void handle_sse_event(const a2a::SseEvent& event);
    void event_thread_func();
};

} // namespace agent_framework

#endif // __AGENT_SSE_CONNECTION_H__
