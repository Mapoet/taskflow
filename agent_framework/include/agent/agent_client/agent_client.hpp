/**
 * @file agent_client.hpp
 * @brief Agent 客户端（A2A 协议）
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_CLIENT_H__
#define __AGENT_CLIENT_H__

#include <atomic>
#include <string>
#include <future>
#include <map>
#include <mutex>
#include <memory>
#include <functional>
#include <optional>
#include <string_view>
#include <agent/core/types.hpp>
#include <agent/agent_transport/sse_connection.hpp>
#include <nlohmann/json.hpp>

namespace agent_framework {
class TokenProvider;

using json = nlohmann::json;

/**
 * @brief Per-instance wire configuration for AgentClient.
 *
 * Unset fields are filled from the process environment once at construction
 * (`AGENT_CLIENT_USE_LEGACY_REST`, `AGENT_CLIENT_JSON_RPC_PATH`). Explicit
 * values override env for that instance only.
 */
struct AgentClientOptions {
    std::optional<bool> use_legacy_rest;
    std::optional<std::string> json_rpc_path;
};

/**
 * @brief HTTP 抽象（JSON GET/POST），具体实现见 HttplibClient
 */
class HTTPClient {
public:
    virtual ~HTTPClient() = default;
    virtual json post(const std::string& url, const json& body,
                      const std::map<std::string, std::string>& headers = {}) = 0;
    virtual json get(const std::string& url,
                     const std::map<std::string, std::string>& headers = {}) = 0;

    /**
     * @brief GET incremental body (SSE / chunked). Stops early if cancel_flag becomes true.
     */
    virtual void get_sse(const std::string& url,
                         const std::map<std::string, std::string>& headers,
                         const std::function<void(std::string_view chunk)>& on_chunk,
                         int timeout_sec,
                         const std::atomic<bool>* cancel_flag) = 0;
};

/**
 * @brief Agent 客户端（A2A 协议）
 * 用于作为客户端与其他 Agent 系统通信
 *
 * @note Wire 模式与 JSON-RPC path 在构造时确定（见 AgentClientOptions 与环境变量）。
 *       默认 JSON-RPC 下 `agent_endpoint` 不参与 RPC URL；Legacy 下用于拼接 `/tasks/…`。
 * @note 返回 `std::future` 的 API 在 **当前调用线程** 内同步完成 HTTP（`std::packaged_task`），
 *       再返回已就绪的 future；避免 libstdc++ 对 `deferred` 使用线程池导致与 httplib/OpenSSL 冲突。
 */
class AgentClient {
public:
    /**
     * @brief 构造函数
     * @param server_url 服务器基础 URL（如 "https://agent.example.com"）
     * @param options 可选；未指定字段在构造时从环境变量读取一次后固化到本实例
     */
    explicit AgentClient(const std::string& server_url,
                         const AgentClientOptions& options = AgentClientOptions{});

    /**
     * @brief 析构函数
     */
    ~AgentClient();

    /**
     * @brief Agent 发现：获取远程 Agent 的 Agent Card
     * @param agent_endpoint Agent 端点 URL（相对或绝对路径，如 "/.well-known/agent-card"）
     * @return Agent Card 的 future
     */
    std::future<AgentCard> discover_agent(const std::string& agent_endpoint);

    /**
     * @brief 发送任务（创建新任务或更新现有任务）
     * @param agent_endpoint Agent 端点 URL
     * @param initial_message 初始消息
     * @param session_id 会话 ID（可选）
     * @param metadata 元数据（可选）
     * @return Agent Task 的 future
     */
    std::future<AgentTask> send_task(
        const std::string& agent_endpoint,
        const AgentMessage& initial_message,
        const std::optional<std::string>& session_id = std::nullopt,
        const json& metadata = {}
    );

    /**
     * @brief 获取任务状态
     * @param agent_endpoint Agent 端点 URL
     * @param task_id 任务 ID
     * @return Agent Task 的 future
     */
    std::future<AgentTask> get_task(const std::string& agent_endpoint, const std::string& task_id);

    /**
     * @brief 取消任务
     * @param agent_endpoint Agent 端点 URL
     * @param task_id 任务 ID
     * @return 是否成功取消的 future
     */
    std::future<bool> cancel_task(const std::string& agent_endpoint, const std::string& task_id);

    /**
     * @brief 更新任务（发送额外输入）
     * @param agent_endpoint Agent 端点 URL
     * @param task_id 任务 ID
     * @param additional_message 额外的消息
     * @return 更新后的 Agent Task 的 future
     */
    std::future<AgentTask> update_task(
        const std::string& agent_endpoint,
        const std::string& task_id,
        const AgentMessage& additional_message
    );

    /**
     * @brief 订阅 SSE 更新
     * @param agent_endpoint Agent 端点 URL
     * @param task_id 任务 ID
     * @param on_status_update 状态更新回调
     * @param on_artifact_update Artifact 更新回调
     */
    void subscribe_task_updates(
        const std::string& agent_endpoint,
        const std::string& task_id,
        std::function<void(const AgentTask&)> on_status_update,
        std::function<void(const AgentArtifact&)> on_artifact_update
    );

    /** SendStreamingMessage over JSON-RPC and consume its SSE stream asynchronously. */
    void send_streaming_task(
        const std::string& agent_endpoint,
        const AgentMessage& initial_message,
        const std::optional<std::string>& session_id,
        const json& metadata,
        std::function<void(const AgentTask&)> on_status_update,
        std::function<void(const AgentArtifact&)> on_artifact_update);

    /**
     * @brief 重新订阅（SSE 连接中断后）
     * @param agent_endpoint Agent 端点 URL
     * @param task_id 任务 ID
     * @param last_event_id 最后接收到的 Event ID
     */
    void resubscribe_task_updates(
        const std::string& agent_endpoint,
        const std::string& task_id,
        const std::string& last_event_id
    );

    /**
     * @brief 设置 Webhook 推送
     * @param agent_endpoint Agent 端点 URL
     * @param task_id 任务 ID
     * @param webhook_url Webhook URL
     */
    void set_push_notification(
        const std::string& agent_endpoint,
        const std::string& task_id,
        const std::string& webhook_url
    );

    /**
     * @brief 获取 Webhook 推送配置
     * @param agent_endpoint Agent 端点 URL
     * @param task_id 任务 ID
     * @return 配置信息的 future
     */
    std::future<json> get_push_notification_config(
        const std::string& agent_endpoint,
        const std::string& task_id
    );

    /**
     * @brief 设置认证配置
     * @param auth_config 认证配置（JSON 对象，如 {"type": "bearer", "token": "..."}）
     */
    void set_authentication(const json& auth_config);

    /**
     * @brief 刷新认证（如刷新 OAuth token）
     */
    void refresh_authentication();

    /** Attach an OAuth-capable provider; it owns refresh timing and credential storage. */
    void set_token_provider(std::shared_ptr<TokenProvider> provider);

    /**
     * @brief 拼接两段 URL 路径（供 HTTPAgentTransport 等复用）
     */
    static std::string join_url(const std::string& base, const std::string& path);

private:
    std::string server_url_;                                            // 服务器基础 URL
    const bool use_legacy_rest_;                                        // Legacy REST vs JSON-RPC（构造时固化）
    const std::string json_rpc_path_;                                   // JSON-RPC POST path（构造时固化）
    json auth_config_;                                                  // 认证配置
    std::shared_ptr<TokenProvider> token_provider_;
    mutable std::mutex auth_mutex_;                                    // 认证互斥锁

    // HTTP 客户端（HttplibClient：REST 与 AgentServer 对齐）
    std::unique_ptr<HTTPClient> http_client_;

    std::atomic<std::uint64_t> jsonrpc_next_id_{1};

    // SSE 连接管理（key: "agent_endpoint:task_id"）
    std::map<std::string, std::unique_ptr<SSEConnection>> sse_connections_;
    mutable std::mutex sse_mutex_;

    /**
     * @brief 构建认证 Header
     * @return Header 键值对
     */
    std::map<std::string, std::string> build_auth_headers() const;

    /**
     * @brief 生成 SSE 连接键
     * @param agent_endpoint Agent 端点
     * @param task_id 任务 ID
     * @return 连接键
     */
    static std::string make_sse_key(const std::string& agent_endpoint, const std::string& task_id);

    /** @brief Append api_key_query param to GET url (caller holds auth_mutex_). */
    std::string append_auth_query_to_get_url_unlocked(const std::string& url) const;
};

} // namespace agent_framework

#endif // __AGENT_CLIENT_H__
