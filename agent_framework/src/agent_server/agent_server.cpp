/**
 * @file agent_server.cpp
 * @brief Agent 服务器实现（A2A 协议）
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#include <agent/agent_server.hpp>
#include <agent/types.hpp>
#include <workflow/nodeflow.hpp>
#include <stdexcept>
#include <sstream>
#include <random>
#include <ctime>
#include <iomanip>
// #include <uuid/uuid.h>  // TODO: 或者使用其他 UUID 生成库（可选）

// 在实现文件中包含 httplib 完整定义
// 检查 httplib 位置并包含
#if __has_include(<httplib/httplib.hpp>)
    #include <httplib/httplib.hpp>
#elif __has_include(<httplib.hpp>)
    #include <httplib.hpp>
#elif __has_include(<httplib.h>)
    #include <httplib.h>
#else
    #error "httplib not found. Please ensure httplib is available in 3rd-party/httplib/"
#endif

namespace agent_framework {

AgentServer::AgentServer(int port)
    : port_(port), http_server_(nullptr) {
    // TODO: 初始化 httplib::Server
    // 在实现时，取消下面的注释：
    // http_server_ = new httplib::Server();
}

AgentServer::~AgentServer() {
    stop();
    // 清理 http_server_
    if (http_server_) {
        // delete static_cast<httplib::Server*>(http_server_);  // TODO: 实现时取消注释
        http_server_ = nullptr;
    }
}

void AgentServer::start() {
    // TODO: 实现 HTTP 服务器启动
    if (!http_server_) {
        // http_server_ = new httplib::Server();  // TODO: 实现时取消注释
        // setup_routes();
    }
    // static_cast<httplib::Server*>(http_server_)->listen("0.0.0.0", port_);
}

void AgentServer::stop() {
    // TODO: 停止 HTTP 服务器
    if (http_server_) {
        // static_cast<httplib::Server*>(http_server_)->stop();  // TODO: 实现时取消注释
    }
    
    // 清理所有 SSE 连接
    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        sse_subscribers_.clear();
    }
}

void AgentServer::register_agent_card(const AgentCard& card) {
    agent_card_ = card;
}

void AgentServer::set_task_handler(
    std::function<std::future<AgentTask>(const AgentTask&, std::shared_ptr<workflow::GraphBuilder>)> handler
) {
    task_handler_ = handler;
}

void AgentServer::set_authentication_validator(
    std::function<bool(const std::map<std::string, std::string>& headers)> validator
) {
    auth_validator_ = validator;
}

void AgentServer::push_task_status_update(const std::string& task_id, const AgentTask& task) {
    std::lock_guard<std::mutex> lock(sse_mutex_);
    
    auto it = sse_subscribers_.find(task_id);
    if (it != sse_subscribers_.end()) {
        // TODO: 通过 SSE 推送状态更新
        json event = {
            {"type", "task_status_update"},
            {"task", task.to_json()}
        };
        
        // 向所有订阅者发送 SSE 事件
        // for (auto& conn : it->second) {
        //     conn->send_event("status", event.dump());
        // }
    }
}

void AgentServer::push_artifact_update(const std::string& task_id, const AgentArtifact& artifact) {
    std::lock_guard<std::mutex> lock(sse_mutex_);
    
    auto it = sse_subscribers_.find(task_id);
    if (it != sse_subscribers_.end()) {
        // TODO: 通过 SSE 推送 Artifact 更新
        json event = {
            {"type", "artifact_update"},
            {"artifact", artifact.to_json()}
        };
        
        // 向所有订阅者发送 SSE 事件
        // for (auto& conn : it->second) {
        //     conn->send_event("artifact", event.dump());
        // }
    }
}

void AgentServer::notify_task_update_via_webhook(const std::string& task_id, const AgentTask& task) {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    
    auto it = webhook_urls_.find(task_id);
    if (it != webhook_urls_.end()) {
        // TODO: 发送 HTTP POST 请求到 Webhook URL
        json payload = {
            {"task_id", task_id},
            {"task", task.to_json()}
        };
        
        // 发送 POST 请求到 it->second（Webhook URL）
    }
}

void AgentServer::setup_routes() {
    if (!http_server_) {
        return;
    }
    
    // TODO: 设置 HTTP 路由
    // 在实现时，取消下面的注释并使用 static_cast 转换：
    // httplib::Server* server = static_cast<httplib::Server*>(http_server_);
    // server->Get("/.well-known/agent-card", [this](const httplib::Request& req, httplib::Response& res) {
    //     handle_well_known_agent_card(res);
    // });
    //
    // http_server_->Post("/tasks/send", [this](const httplib::Request& req, httplib::Response& res) {
    //     handle_tasks_send(req, res);
    // });
    //
    // http_server_->Get("/tasks/get", [this](const httplib::Request& req, httplib::Response& res) {
    //     handle_tasks_get(req, res);
    // });
    //
    // http_server_->Post("/tasks/cancel", [this](const httplib::Request& req, httplib::Response& res) {
    //     handle_tasks_cancel(req, res);
    // });
    //
    // server->Post("/tasks/update", [this](const httplib::Request& req, httplib::Response& res) {
    //     handle_tasks_update(req, res);
    // });
    //
    // http_server_->Get("/tasks/sendSubscribe", [this](const httplib::Request& req, httplib::Response& res) {
    //     handle_tasks_send_subscribe(req, res);
    // });
    //
    // http_server_->Post("/tasks/resubscribe", [this](const httplib::Request& req, httplib::Response& res) {
    //     handle_tasks_resubscribe(req, res);
    // });
    //
    // http_server_->Post("/tasks/pushNotification/set", [this](const httplib::Request& req, httplib::Response& res) {
    //     handle_push_notification_set(req, res);
    // });
    //
    // http_server_->Get("/tasks/pushNotification/get", [this](const httplib::Request& req, httplib::Response& res) {
    //     handle_push_notification_get(req, res);
    // });
}

void AgentServer::handle_well_known_agent_card(httplib::Response& res) {
    res.set_content(agent_card_.to_json().dump(), "application/json");
}

void AgentServer::handle_tasks_send(const httplib::Request& req, httplib::Response& res) {
    if (!validate_authentication(req)) {
        res.status = 401;
        res.set_content(json{{"error", "Unauthorized"}}.dump(), "application/json");
        return;
    }
    
    try {
        json request = json::parse(req.body);
        
        // 解析请求参数
        AgentMessage initial_message = AgentMessage::from_json(request["message"]);
        std::optional<std::string> session_id;
        if (request.contains("session_id")) {
            session_id = request["session_id"].get<std::string>();
        }
        json metadata = request.value("metadata", json::object());
        
        // 创建任务
        AgentTask task;
        task.task_id = generate_task_id();
        task.session_id = session_id;
        task.status = AgentTaskStatus::PENDING;
        task.messages.push_back(initial_message);
        task.metadata = metadata;
        task.created_at = std::chrono::system_clock::now();
        task.updated_at = task.created_at;
        
        // 保存任务
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            active_tasks_[task.task_id] = task;
        }
        
        // 如果有任务处理器，异步执行
        if (task_handler_) {
            // TODO: 创建 workflow::GraphBuilder
            // auto builder = std::make_shared<workflow::GraphBuilder>("AgentTask_" + task.task_id);
            // auto future = task_handler_(task, builder);
            // future.wait();
        }
        
        // 返回任务
        res.set_content(json{{"task", task.to_json()}}.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

void AgentServer::handle_tasks_get(const httplib::Request& req, httplib::Response& res) {
    if (!validate_authentication(req)) {
        res.status = 401;
        res.set_content(json{{"error", "Unauthorized"}}.dump(), "application/json");
        return;
    }
    
    std::string task_id = req.get_param_value("task_id");
    
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    auto it = active_tasks_.find(task_id);
    
    if (it == active_tasks_.end()) {
        res.status = 404;
        res.set_content(json{{"error", "Task not found"}}.dump(), "application/json");
        return;
    }
    
    res.set_content(json{{"task", it->second.to_json()}}.dump(), "application/json");
}

void AgentServer::handle_tasks_cancel(const httplib::Request& req, httplib::Response& res) {
    if (!validate_authentication(req)) {
        res.status = 401;
        res.set_content(json{{"error", "Unauthorized"}}.dump(), "application/json");
        return;
    }
    
    try {
        json request = json::parse(req.body);
        std::string task_id = request["task_id"].get<std::string>();
        
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        auto it = active_tasks_.find(task_id);
        
        if (it != active_tasks_.end()) {
            it->second.status = AgentTaskStatus::CANCELLED;
            it->second.updated_at = std::chrono::system_clock::now();
            
            res.set_content(json{{"success", true}}.dump(), "application/json");
        } else {
            res.status = 404;
            res.set_content(json{{"error", "Task not found"}}.dump(), "application/json");
        }
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

void AgentServer::handle_tasks_update(const httplib::Request& req, httplib::Response& res) {
    if (!validate_authentication(req)) {
        res.status = 401;
        res.set_content(json{{"error", "Unauthorized"}}.dump(), "application/json");
        return;
    }

    try {
        json request = json::parse(req.body);
        std::string task_id = request["task_id"].get<std::string>();
        AgentMessage additional = AgentMessage::from_json(request["message"]);

        std::lock_guard<std::mutex> lock(tasks_mutex_);
        auto it = active_tasks_.find(task_id);
        if (it == active_tasks_.end()) {
            res.status = 404;
            res.set_content(json{{"error", "Task not found"}}.dump(), "application/json");
            return;
        }

        it->second.messages.push_back(additional);
        it->second.updated_at = std::chrono::system_clock::now();
        res.set_content(json{{"task", it->second.to_json()}}.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

void AgentServer::handle_tasks_send_subscribe(const httplib::Request& req, httplib::Response& res) {
    if (!validate_authentication(req)) {
        res.status = 401;
        return;
    }
    
    std::string task_id = req.get_param_value("task_id");
    
    // TODO: 设置 SSE 响应头
    // res.set_header("Content-Type", "text/event-stream");
    // res.set_header("Cache-Control", "no-cache");
    // res.set_header("Connection", "keep-alive");
    
    // TODO: 创建 SSE 连接并添加到订阅者列表
    // auto sse_conn = std::make_shared<SSEConnection>(...);
    // {
    //     std::lock_guard<std::mutex> lock(sse_mutex_);
    //     sse_subscribers_[task_id].push_back(sse_conn);
    // }
}

void AgentServer::handle_tasks_resubscribe(const httplib::Request& req, httplib::Response& res) {
    if (!validate_authentication(req)) {
        res.status = 401;
        return;
    }
    
    try {
        json request = json::parse(req.body);
        std::string task_id = request["task_id"].get<std::string>();
        std::string last_event_id = request.value("last_event_id", "");
        
        // TODO: 重新连接 SSE
        // 参考 handle_tasks_send_subscribe
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

void AgentServer::handle_push_notification_set(const httplib::Request& req, httplib::Response& res) {
    if (!validate_authentication(req)) {
        res.status = 401;
        return;
    }
    
    try {
        json request = json::parse(req.body);
        std::string task_id = request["task_id"].get<std::string>();
        std::string webhook_url = request["webhook_url"].get<std::string>();
        
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        webhook_urls_[task_id] = webhook_url;
        
        res.set_content(json{{"success", true}}.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

void AgentServer::handle_push_notification_get(const httplib::Request& req, httplib::Response& res) {
    if (!validate_authentication(req)) {
        res.status = 401;
        return;
    }
    
    std::string task_id = req.get_param_value("task_id");
    
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    auto it = webhook_urls_.find(task_id);
    
    if (it != webhook_urls_.end()) {
        res.set_content(json{{"webhook_url", it->second}}.dump(), "application/json");
    } else {
        res.status = 404;
        res.set_content(json{{"error", "Webhook not found"}}.dump(), "application/json");
    }
}

bool AgentServer::validate_authentication(const httplib::Request& /* req */) {
    if (!auth_validator_) {
        return true;  // 如果没有设置验证器，默认通过
    }
    
    std::map<std::string, std::string> headers;
    // TODO: 从 req 提取 headers
    // for (const auto& header : req.headers) {
    //     headers[header.first] = header.second;
    // }
    
    return auth_validator_(headers);
}

std::string AgentServer::generate_task_id() {
    // TODO: 使用 UUID 生成唯一任务 ID
    // 临时使用简单实现
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    
    std::ostringstream oss;
    oss << "task_";
    for (int i = 0; i < 32; ++i) {
        oss << std::hex << dis(gen);
    }
    return oss.str();
}

} // namespace agent_framework

