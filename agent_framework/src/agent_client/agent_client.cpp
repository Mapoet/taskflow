/**
 * @file agent_client.cpp
 * @brief Agent 客户端实现（A2A 协议）
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#include <agent/agent_client.hpp>
#include <agent/types.hpp>
#include <stdexcept>
#include <sstream>
#include <linux/uuid.h>  // TODO: 或者使用其他 UUID 生成库

// TODO: 实现 HTTPClient 或使用 httplib

namespace agent_framework {

// TODO: 实现具体的 HTTPClient（使用 httplib 或 curl）
// 临时实现类
class SimpleHTTPClient : public HTTPClient {
public:
    json post(const std::string& /* url */, const json& /* body */, const std::map<std::string, std::string>& /* headers */ = {}) override {
        // TODO: 使用 httplib 或 curl 实现 HTTP POST
        throw std::runtime_error("HTTPClient::post not implemented. Please use httplib or curl.");
    }
};

AgentClient::AgentClient(const std::string& server_url)
    : server_url_(server_url), http_client_(std::make_unique<SimpleHTTPClient>()) {
    // 初始化空认证配置
    auth_config_ = json::object();
}

AgentClient::~AgentClient() {
    // 清理 SSE 连接
    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        sse_connections_.clear();
    }
}

std::future<AgentCard> AgentClient::discover_agent(const std::string& agent_endpoint) {
    return std::async(std::launch::async, [this, agent_endpoint]() {
        // TODO: 实现 Agent 发现
        // 1. 构建完整 URL: server_url_ + agent_endpoint（通常是 "/.well-known/agent-card"）
        // 2. 发送 GET 请求
        // 3. 解析响应 JSON，返回 AgentCard
        
        json response = send_jsonrpc_request(agent_endpoint, "discover", json::object());
        return AgentCard::from_json(response["result"]);
    });
}

std::future<AgentTask> AgentClient::send_task(
    const std::string& agent_endpoint,
    const AgentMessage& initial_message,
    const std::optional<std::string>& session_id,
    const json& metadata
) {
    return std::async(std::launch::async, [this, agent_endpoint, initial_message, session_id, metadata]() {
        json params = {
            {"message", initial_message.to_json()},
            {"metadata", metadata}
        };
        
        if (session_id.has_value()) {
            params["session_id"] = *session_id;
        }
        
        json response = send_jsonrpc_request(agent_endpoint + "/tasks/send", "send_task", params);
        return AgentTask::from_json(response["result"]);
    });
}

std::future<AgentTask> AgentClient::get_task(const std::string& agent_endpoint, const std::string& task_id) {
    return std::async(std::launch::async, [this, agent_endpoint, task_id]() {
        json params = {{"task_id", task_id}};
        json response = send_jsonrpc_request(agent_endpoint + "/tasks/get", "get_task", params);
        return AgentTask::from_json(response["result"]);
    });
}

std::future<bool> AgentClient::cancel_task(const std::string& agent_endpoint, const std::string& task_id) {
    return std::async(std::launch::async, [this, agent_endpoint, task_id]() {
        json params = {{"task_id", task_id}};
        json response = send_jsonrpc_request(agent_endpoint + "/tasks/cancel", "cancel_task", params);
        return response["result"].get<bool>();
    });
}

std::future<AgentTask> AgentClient::update_task(
    const std::string& agent_endpoint,
    const std::string& task_id,
    const AgentMessage& additional_message
) {
    return std::async(std::launch::async, [this, agent_endpoint, task_id, additional_message]() {
        json params = {
            {"task_id", task_id},
            {"message", additional_message.to_json()}
        };
        json response = send_jsonrpc_request(agent_endpoint + "/tasks/update", "update_task", params);
        return AgentTask::from_json(response["result"]);
    });
}

void AgentClient::subscribe_task_updates(
    const std::string& agent_endpoint,
    const std::string& task_id,
    std::function<void(const AgentTask&)> on_status_update,
    std::function<void(const AgentArtifact&)> on_artifact_update
) {
    std::lock_guard<std::mutex> lock(sse_mutex_);
    
    std::string sse_key = make_sse_key(agent_endpoint, task_id);
    
    // 构建 SSE 端点 URL
    std::ostringstream oss;
    oss << server_url_ << agent_endpoint << "/tasks/subscribe?task_id=" << task_id;
    std::string sse_endpoint = oss.str();
    
    // 创建 SSE 连接
    auto sse_conn = std::make_unique<SSEConnection>(sse_endpoint, task_id);
    sse_conn->subscribe(on_status_update, on_artifact_update);
    
    sse_connections_[sse_key] = std::move(sse_conn);
}

void AgentClient::resubscribe_task_updates(
    const std::string& agent_endpoint,
    const std::string& task_id,
    const std::string& last_event_id
) {
    std::lock_guard<std::mutex> lock(sse_mutex_);
    
    std::string sse_key = make_sse_key(agent_endpoint, task_id);
    
    auto it = sse_connections_.find(sse_key);
    if (it != sse_connections_.end()) {
        it->second->reconnect(last_event_id);
    }
}

void AgentClient::set_push_notification(
    const std::string& agent_endpoint,
    const std::string& task_id,
    const std::string& webhook_url
) {
    json params = {
        {"task_id", task_id},
        {"webhook_url", webhook_url}
    };
    send_jsonrpc_request(agent_endpoint + "/tasks/pushNotification/set", "set_push_notification", params);
}

std::future<json> AgentClient::get_push_notification_config(
    const std::string& agent_endpoint,
    const std::string& task_id
) {
    return std::async(std::launch::async, [this, agent_endpoint, task_id]() {
        json params = {{"task_id", task_id}};
        json response = send_jsonrpc_request(agent_endpoint + "/tasks/pushNotification/get", "get_push_notification", params);
        return response["result"];
    });
}

void AgentClient::set_authentication(const json& auth_config) {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    auth_config_ = auth_config;
}

void AgentClient::refresh_authentication() {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    // TODO: 实现认证刷新逻辑（如 OAuth token 刷新）
}

json AgentClient::send_jsonrpc_request(const std::string& endpoint, const json& method, const json& params) {
    // 构建完整 URL
    std::string full_url = server_url_ + endpoint;
    
    // 构建 JSON-RPC 2.0 请求
    json request = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params},
        {"id", 1}  // TODO: 使用唯一 ID
    };
    
    // 构建请求头
    auto headers = build_auth_headers();
    headers["Content-Type"] = "application/json";
    
    // 发送请求
    json response = http_client_->post(full_url, request, headers);
    
    // 检查错误
    if (response.contains("error")) {
        throw std::runtime_error("JSON-RPC error: " + response["error"]["message"].get<std::string>());
    }
    
    return response;
}

std::map<std::string, std::string> AgentClient::build_auth_headers() const {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    
    std::map<std::string, std::string> headers;
    
    if (!auth_config_.empty()) {
        std::string auth_type = auth_config_.value("type", "");
        
        if (auth_type == "bearer") {
            std::string token = auth_config_.value("token", "");
            headers["Authorization"] = "Bearer " + token;
        } else if (auth_type == "api_key") {
            std::string key_name = auth_config_.value("key_name", "X-API-Key");
            std::string key_value = auth_config_.value("key_value", "");
            headers[key_name] = key_value;
        }
        // TODO: 支持其他认证方式
    }
    
    return headers;
}

std::string AgentClient::make_sse_key(const std::string& agent_endpoint, const std::string& task_id) {
    return agent_endpoint + ":" + task_id;
}

} // namespace agent_framework

