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
#include <agent/types.hpp>
#include <agent/sse_connection.hpp>
#include <nlohmann/json.hpp>

// 前向声明 workflow 和 httplib
namespace workflow {
    class GraphBuilder;
}

namespace httplib {
    class Server;
    class Request;
    class Response;
}

namespace agent_framework {
    
using json = nlohmann::json;

/**
 * @brief Agent 服务器（A2A 协议）
 * 用于对外提供 A2A 协议接口，使本框架的 Agent 能够被其他系统发现和调用
 */
class AgentServer {
public:
    /**
     * @brief 构造函数
     * @param port 服务器端口（默认 8080）
     */
    explicit AgentServer(int port = 8080);
    
    /**
     * @brief 析构函数
     */
    ~AgentServer();
    
    /**
     * @brief 启动服务器（阻塞调用）
     */
    void start();
    
    /**
     * @brief 停止服务器
     */
    void stop();
    
    /**
     * @brief 注册本 Agent 的 Agent Card
     * @param card Agent Card
     */
    void register_agent_card(const AgentCard& card);
    
    /**
     * @brief 设置任务处理器（将 Agent Task 转换为 workflow 执行）
     * @param handler 任务处理器函数
     * 
     * handler 签名：
     * ```cpp
     * std::future<AgentTask>(const AgentTask& task, std::shared_ptr<workflow::GraphBuilder> builder)
     * ```
     */
    void set_task_handler(
        std::function<std::future<AgentTask>(
            const AgentTask& task,
            std::shared_ptr<workflow::GraphBuilder> builder
        )> handler
    );
    
    /**
     * @brief 设置认证验证器
     * @param validator 验证器函数
     * 
     * validator 签名：
     * ```cpp
     * bool(const std::map<std::string, std::string>& headers)
     * ```
     */
    void set_authentication_validator(
        std::function<bool(const std::map<std::string, std::string>& headers)> validator
    );
    
    /**
     * @brief 推送任务状态更新（通过 SSE）
     * @param task_id 任务 ID
     * @param task 任务对象
     */
    void push_task_status_update(const std::string& task_id, const AgentTask& task);
    
    /**
     * @brief 推送 Artifact 更新（通过 SSE）
     * @param task_id 任务 ID
     * @param artifact Artifact 对象
     */
    void push_artifact_update(const std::string& task_id, const AgentArtifact& artifact);
    
    /**
     * @brief 通过 Webhook 通知任务更新
     * @param task_id 任务 ID
     * @param task 任务对象
     */
    void notify_task_update_via_webhook(const std::string& task_id, const AgentTask& task);
    
private:
    int port_;                                                              // 服务器端口
    std::unique_ptr<httplib::Server> http_server_;                          // HTTP 服务器
    AgentCard agent_card_;                                                  // Agent Card
    std::map<std::string, AgentTask> active_tasks_;                        // 活动任务（key: task_id）
    std::map<std::string, std::vector<std::shared_ptr<SSEConnection>>> sse_subscribers_;  // SSE 订阅者（key: task_id）
    std::map<std::string, std::string> webhook_urls_;                      // Webhook URL（key: task_id）
    mutable std::mutex tasks_mutex_;                                       // 任务互斥锁
    mutable std::mutex sse_mutex_;                                         // SSE 互斥锁
    
    // 任务处理器（将 Agent Task 转换为 workflow）
    std::function<std::future<AgentTask>(const AgentTask&, std::shared_ptr<workflow::GraphBuilder>)> task_handler_;
    
    // 认证验证器
    std::function<bool(const std::map<std::string, std::string>&)> auth_validator_;
    
    /**
     * @brief 设置 HTTP 路由
     */
    void setup_routes();
    
    /**
     * @brief 处理 /.well-known/agent-card 请求
     * @param res HTTP 响应
     */
    void handle_well_known_agent_card(httplib::Response& res);
    
    /**
     * @brief 处理 /tasks/send 请求（创建/更新任务）
     * @param req HTTP 请求
     * @param res HTTP 响应
     */
    void handle_tasks_send(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief 处理 /tasks/get 请求（获取任务状态）
     * @param req HTTP 请求
     * @param res HTTP 响应
     */
    void handle_tasks_get(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief 处理 /tasks/cancel 请求（取消任务）
     * @param req HTTP 请求
     * @param res HTTP 响应
     */
    void handle_tasks_cancel(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief 处理 /tasks/sendSubscribe 请求（订阅 SSE 更新）
     * @param req HTTP 请求
     * @param res HTTP 响应
     */
    void handle_tasks_send_subscribe(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief 处理 /tasks/resubscribe 请求（重新订阅）
     * @param req HTTP 请求
     * @param res HTTP 响应
     */
    void handle_tasks_resubscribe(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief 处理 /tasks/pushNotification/set 请求（设置 Webhook）
     * @param req HTTP 请求
     * @param res HTTP 响应
     */
    void handle_push_notification_set(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief 处理 /tasks/pushNotification/get 请求（获取 Webhook 配置）
     * @param req HTTP 请求
     * @param res HTTP 响应
     */
    void handle_push_notification_get(const httplib::Request& req, httplib::Response& res);
    
    /**
     * @brief 验证请求认证
     * @param req HTTP 请求
     * @return 是否认证通过
     */
    bool validate_authentication(const httplib::Request& req);
    
    /**
     * @brief 生成唯一任务 ID
     * @return 任务 ID
     */
    static std::string generate_task_id();
};

} // namespace agent_framework

#endif // __AGENT_SERVER_H__

