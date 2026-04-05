/**
 * @file agent_server.hpp
 * @brief Agent 服务器（A2A 协议）
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_SERVER_H__
#define __AGENT_SERVER_H__

#include <string>
#include <future>
#include <map>
#include <vector>
#include <mutex>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <agent/types.hpp>
#include <nlohmann/json.hpp>

namespace workflow {
class GraphBuilder;
}

namespace tf {
class Executor;
}

namespace httplib {
class Server;
class Request;
class Response;
}

namespace agent_framework {

namespace internal {
class TaskDispatchQueue;
class SseServerChannel;
}

namespace a2a {
class DispatchTable;
}

using json = nlohmann::json;

/**
 * @brief Agent 服务器（A2A 协议）
 * 用于对外提供 A2A 协议接口，使本框架的 Agent 能够被其他系统发现和调用
 */
class AgentServer {
public:
    /**
     * @brief 构造函数
     * @param port 服务器端口（默认 8080）；传入 0 时 start() 使用系统分配临时端口（见 bound_port()）
     */
    explicit AgentServer(int port = 8080);

    ~AgentServer();

    AgentServer(const AgentServer&) = delete;
    AgentServer& operator=(const AgentServer&) = delete;

    /**
     * @brief 启动服务器（阻塞调用 listen）
     */
    void start();

    /**
     * @brief 停止服务器
     */
    void stop();

    /** @brief listen 成功后实际绑定的端口（port==0 时有效） */
    int bound_port() const { return bound_port_; }

    /**
     * @brief 注册本 Agent 的 Agent Card
     * @param card Agent Card
     */
    void register_agent_card(const AgentCard& card);

    /**
     * @brief 设置任务处理器（将 Agent Task 转换为 workflow 执行）
     */
    void set_task_handler(
        std::function<std::future<AgentTask>(
            const AgentTask& task,
            std::shared_ptr<workflow::GraphBuilder> builder
        )> handler
    );

    /**
     * @brief 设置认证验证器
     */
    void set_authentication_validator(
        std::function<bool(const std::map<std::string, std::string>& headers)> validator
    );

    void push_task_status_update(const std::string& task_id, const AgentTask& task);

    void push_artifact_update(const std::string& task_id, const AgentArtifact& artifact);

    void notify_task_update_via_webhook(const std::string& task_id, const AgentTask& task);

private:
    int port_;
    int bound_port_{-1};
    void* http_server_{nullptr};
    AgentCard agent_card_;
    std::map<std::string, AgentTask> active_tasks_;
    std::map<std::string, std::vector<std::shared_ptr<internal::SseServerChannel>>> sse_subscribers_;
    std::map<std::string, std::string> webhook_urls_;
    mutable std::mutex tasks_mutex_;
    mutable std::mutex sse_mutex_;

    std::function<std::future<AgentTask>(const AgentTask&, std::shared_ptr<workflow::GraphBuilder>)>
        task_handler_;
    std::function<bool(const std::map<std::string, std::string>&)> auth_validator_;

    std::unique_ptr<internal::TaskDispatchQueue> task_queue_;
    std::vector<std::thread> dispatch_workers_;
    std::shared_ptr<tf::Executor> process_executor_;
    std::unique_ptr<a2a::DispatchTable> rpc_dispatch_;
    std::atomic<bool> routes_ready_{false};
    std::atomic<bool> workers_started_{false};

    void ensure_runtime();
    void start_dispatch_workers();
    void dispatch_worker_loop();
    void run_agent_task_on_executor(const std::string& task_id,
                                    AgentTask task_snapshot,
                                    std::shared_ptr<workflow::GraphBuilder> builder);

    void setup_routes();
    void register_jsonrpc_methods();

    void handle_well_known_agent_card(httplib::Response& res);
    void handle_health(httplib::Response& res);

    void handle_jsonrpc_post(const httplib::Request& req, httplib::Response& res);

    void handle_tasks_send(const httplib::Request& req, httplib::Response& res);
    void handle_tasks_get(const httplib::Request& req, httplib::Response& res);
    void handle_tasks_cancel(const httplib::Request& req, httplib::Response& res);
    void handle_tasks_update(const httplib::Request& req, httplib::Response& res);
    void handle_tasks_send_subscribe(const httplib::Request& req, httplib::Response& res);
    void handle_tasks_resubscribe(const httplib::Request& req, httplib::Response& res);
    void handle_push_notification_set(const httplib::Request& req, httplib::Response& res);
    void handle_push_notification_get(const httplib::Request& req, httplib::Response& res);

    bool validate_authentication(const httplib::Request& req);

    json jsonrpc_send_message(const json& params);
    json jsonrpc_get_task(const json& params);
    json jsonrpc_cancel_task(const json& params);
    json jsonrpc_list_tasks(const json& params);

    AgentTask create_task_from_message_wire(const AgentMessage& initial_message,
                                            const std::optional<std::string>& session_id,
                                            const json& metadata);

    static std::string generate_task_id();
    static std::map<std::string, std::string> lower_headers(const httplib::Request& req);

    void remove_sse_channel(const std::string& task_id,
                            const std::shared_ptr<internal::SseServerChannel>& ch);
};

} // namespace agent_framework

#endif // __AGENT_SERVER_H__
