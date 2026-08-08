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
#include <agent/a2a/auth_gate.hpp>
#include <agent/core/types.hpp>
#include <agent/agent/task_state_machine.hpp>
#include <agent/graph_executor/graph_executor.hpp>
#include <agent/session/session_store.hpp>
#include <nlohmann/json.hpp>

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

class ToolBus;

struct AgentExecutionProfile {
    std::string template_id = kWorkflowTemplateReactCli;
    AgentConfig config;
    AgentWorkflowDeps deps;
    InputPolicyConfig input_policy;
};

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

    void set_execution_profile(AgentExecutionProfile profile);
    void set_graph_executor(std::shared_ptr<GraphExecutor> executor);
    void set_session_store(std::shared_ptr<SessionStore> store);

    /**
     * @brief 设置附加认证验证器（在内置 AuthGate 通过后与关系 AND）
     */
    void set_authentication_validator(std::function<bool(const a2a::AuthContext& ctx)> validator);

    /**
     * @brief WP2.7：@file/@url 物化所需 ToolBus；为 null 且用户输入触发注入时会产生 violation
     */
    void set_input_preprocess_toolbus(std::shared_ptr<ToolBus> toolbus);

    void push_task_status_update(const std::string& task_id, const AgentTask& task);

    void push_artifact_update(const std::string& task_id, const AgentArtifact& artifact);

    /** Push an A2A StreamResponse.message append delta for an active task. */
    void push_message_delta(const std::string& task_id, std::string_view text,
                            std::string_view channel = "answer");

    /**
     * @brief WP2.8：向订阅方推送 Verifier 子事件（SSE event 名如 verifier_started / verifier_completed）
     * @param payload 业务字段；实现中会填入 component=verifier 后再序列化
     */
    void push_verifier_sse(const std::string& task_id,
                          std::string_view sse_event_name,
                          const json& payload);

    void notify_task_update_via_webhook(const std::string& task_id, const AgentTask& task);

private:
    int port_;
    int bound_port_{-1};
    void* http_server_{nullptr};
    AgentCard agent_card_;
    std::map<std::string, AgentTask> active_tasks_;
    std::map<std::string, std::shared_ptr<TaskControl>> task_controls_;
    std::map<std::string, std::vector<std::shared_ptr<internal::SseServerChannel>>> sse_subscribers_;
    std::map<std::string, std::string> webhook_urls_;
    mutable std::mutex tasks_mutex_;
    mutable std::mutex sse_mutex_;

    std::function<bool(const a2a::AuthContext&)> auth_validator_;
    a2a::AuthGateConfig auth_gate_config_;
    std::shared_ptr<ToolBus> preprocess_toolbus_;
    std::optional<AgentExecutionProfile> execution_profile_;
    std::shared_ptr<GraphExecutor> graph_executor_;
    std::shared_ptr<SessionStore> session_store_;

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
                                    std::shared_ptr<TaskControl> control);

    void setup_routes();
    void register_jsonrpc_methods();

    void handle_well_known_agent_card(const httplib::Request& req, httplib::Response& res);
    void handle_health(httplib::Response& res);

    void handle_jsonrpc_post(const httplib::Request& req, httplib::Response& res);

    void handle_tasks_send(const httplib::Request& req, httplib::Response& res);
    void handle_tasks_get(const httplib::Request& req, httplib::Response& res);
    void handle_tasks_cancel(const httplib::Request& req, httplib::Response& res);
    void handle_tasks_update(const httplib::Request& req, httplib::Response& res);
    void handle_tasks_send_subscribe(const httplib::Request& req, httplib::Response& res);
    void attach_task_stream(const std::string& task_id, httplib::Response& res);
    void handle_tasks_resubscribe(const httplib::Request& req, httplib::Response& res);
    void handle_push_notification_set(const httplib::Request& req, httplib::Response& res);
    void handle_push_notification_get(const httplib::Request& req, httplib::Response& res);

    bool apply_auth_gate(const httplib::Request& req, httplib::Response& res);

    json jsonrpc_send_message(const json& params);
    json jsonrpc_get_task(const json& params);
    json jsonrpc_cancel_task(const json& params);
    json jsonrpc_list_tasks(const json& params);

    AgentTask create_task_from_message_wire(const AgentMessage& initial_message,
                                            const std::optional<std::string>& session_id,
                                            const json& metadata);

    static std::string generate_task_id();
    void remove_sse_channel(const std::string& task_id,
                            const std::shared_ptr<internal::SseServerChannel>& ch);
    void push_execution_status_update(const std::string& task_id, const json& metadata);
};

} // namespace agent_framework

#endif // __AGENT_SERVER_H__
