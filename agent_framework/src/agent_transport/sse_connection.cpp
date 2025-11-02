/**
 * @file sse_connection.cpp
 * @brief SSE 连接管理器实现（A2A 协议）
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#include <agent/sse_connection.hpp>
#include <iostream>
#include <sstream>
#include <nlohmann/json.hpp>

// TODO: 实现 SSEConnection
// 需要引入实际的 HTTP 客户端库（如 httplib）来接收 SSE 流

namespace agent_framework {

SSEConnection::SSEConnection(const std::string& endpoint, const std::string& task_id)
    : endpoint_(endpoint), task_id_(task_id), active_(false) {
    // TODO: 初始化 SSE 连接
}

SSEConnection::~SSEConnection() {
    close();
    if (event_thread_.joinable()) {
        event_thread_.join();
    }
}

void SSEConnection::subscribe(
    std::function<void(const AgentTask&)> on_status_update,
    std::function<void(const AgentArtifact&)> on_artifact_update
) {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    
    on_status_update_ = on_status_update;
    on_artifact_update_ = on_artifact_update;
    
    // TODO: 启动 SSE 连接
    // 1. 发送 GET 请求到 endpoint_，设置 Accept: text/event-stream
    // 2. 创建 event_stream_ 响应对象
    // 3. 启动 event_thread_ 处理 SSE 事件流
    
    active_ = true;
    
    // 启动事件处理线程
    event_thread_ = std::thread(&SSEConnection::event_thread_func, this);
}

void SSEConnection::reconnect(const std::string& last_event_id) {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    
    close();
    
    // TODO: 使用 last_event_id 重新连接
    // 在 GET 请求中添加 Last-Event-ID Header
    
    active_ = true;
    event_thread_ = std::thread(&SSEConnection::event_thread_func, this);
}

void SSEConnection::close() {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    
    if (!active_) {
        return;
    }
    
    active_ = false;
    // TODO: 关闭 SSE 连接
    event_stream_.reset();
}

bool SSEConnection::is_active() const {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    return active_;
}

void SSEConnection::handle_event(const std::string& event_data) {
    try {
        // TODO: 解析 SSE 事件数据（JSON 格式）
        // 根据事件类型调用相应的回调
        
        json event_json = json::parse(event_data);
        std::string event_type = event_json.value("type", "");
        
        if (event_type == "task_status_update") {
            if (on_status_update_) {
                AgentTask task = AgentTask::from_json(event_json["task"]);
                on_status_update_(task);
            }
        } else if (event_type == "artifact_update") {
            if (on_artifact_update_) {
                AgentArtifact artifact = AgentArtifact::from_json(event_json["artifact"]);
                on_artifact_update_(artifact);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error handling SSE event: " << e.what() << std::endl;
    }
}

void SSEConnection::event_thread_func() {
    // TODO: 实现 SSE 事件流处理
    // 1. 从 event_stream_ 读取 SSE 格式的事件
    // 2. 解析每个事件（格式：data: {...}\n\n）
    // 3. 调用 handle_event 处理事件
    
    while (active_) {
        // TODO: 读取 SSE 事件流
        // std::string line;
        // if (读取一行) {
        //     if (line.starts_with("data: ")) {
        //         std::string data = line.substr(6);
        //         handle_event(data);
        //     }
        // }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace agent_framework

