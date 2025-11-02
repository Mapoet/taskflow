/**
 * @file sse_connection.hpp
 * @brief SSE 连接管理器（A2A 协议）
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_SSE_CONNECTION_H__
#define __AGENT_SSE_CONNECTION_H__

#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <memory>
#include <agent/types.hpp>
#include <nlohmann/json.hpp>

// 注意：event_stream_ 使用 void* 存储，在实现文件中转换为具体类型
// 这样可以避免在头文件中包含 httplib.hpp

namespace agent_framework {
    using json = nlohmann::json;
}

namespace agent_framework {

/**
 * @brief SSE 连接管理器
 * 用于管理 Server-Sent Events 连接，实现异步任务更新推送
 */
class SSEConnection {
public:
    /**
     * @brief 构造函数
     * @param endpoint SSE 端点 URL
     * @param task_id 任务 ID
     */
    explicit SSEConnection(const std::string& endpoint, const std::string& task_id);
    
    /**
     * @brief 析构函数
     */
    ~SSEConnection();
    
    /**
     * @brief 订阅 SSE 事件流
     * @param on_status_update 状态更新回调
     * @param on_artifact_update Artifact 更新回调
     */
    void subscribe(
        std::function<void(const AgentTask&)> on_status_update,
        std::function<void(const AgentArtifact&)> on_artifact_update
    );
    
    /**
     * @brief 重新连接（连接中断后）
     * @param last_event_id 最后接收到的 Event ID（用于断点续传）
     */
    void reconnect(const std::string& last_event_id);
    
    /**
     * @brief 关闭连接
     */
    void close();
    
    /**
     * @brief 检查连接状态
     * @return 是否活跃
     */
    bool is_active() const;
    
private:
    std::string endpoint_;                              // SSE 端点 URL
    std::string task_id_;                               // 任务 ID
    void* event_stream_;                                // SSE 响应流（httplib::Response*，在实现文件中转换为具体类型）
    bool active_ = false;                               // 连接状态
    std::thread event_thread_;                          // 事件处理线程
    mutable std::mutex connection_mutex_;               // 连接互斥锁（mutable 以支持 const 方法）
    
    std::function<void(const AgentTask&)> on_status_update_;        // 状态更新回调
    std::function<void(const AgentArtifact&)> on_artifact_update_; // Artifact 更新回调
    
    /**
     * @brief 处理 SSE 事件
     * @param event_data 事件数据（JSON 字符串）
     */
    void handle_event(const std::string& event_data);
    
    /**
     * @brief 事件处理线程主函数
     */
    void event_thread_func();
};

} // namespace agent_framework

#endif // __AGENT_SSE_CONNECTION_H__

