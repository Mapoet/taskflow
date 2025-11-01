/**
 * @file ui_manager.hpp
 * @brief UI Manager 模块：多客户端输出适配
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_UI_MANAGER_H__
#define __AGENT_UI_MANAGER_H__

#include "types.hpp"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <mutex>
#include <iostream>
#include <ostream>
#include <queue>

// 前向声明（避免暴露具体实现细节）
template<typename T>
class ThreadSafeQueue;

namespace agent_framework {

// ============================================================================
// UI 处理器接口
// ============================================================================

/**
 * @brief UI 处理器虚基类
 * 定义统一的 UI 输出接口
 */
class UIHandler {
public:
    virtual ~UIHandler() = default;
    
    /**
     * @brief 处理流式 token
     * @param token token 内容
     */
    virtual void handle_stream_token(std::string_view token) = 0;
    
    /**
     * @brief 处理最终结果
     * @param result 最终结果（JSON 格式）
     */
    virtual void handle_final_result(const json& result) = 0;
    
    /**
     * @brief 处理错误
     * @param error_message 错误消息
     */
    virtual void handle_error(const std::string& error_message) = 0;
    
    /**
     * @brief 获取处理器类型
     * @return 处理器类型（"cli", "imgui", "web"）
     */
    virtual std::string get_handler_type() const = 0;
    
    /**
     * @brief 检查处理器是否活跃
     * @return true 如果处理器活跃
     */
    virtual bool is_active() const = 0;
};

/**
 * @brief CLI 处理器实现
 */
class CLIHandler : public UIHandler {
public:
    /**
     * @brief 构造函数
     * @param output_stream 输出流（默认为 std::cout）
     */
    CLIHandler(std::ostream& output_stream = std::cout);
    
    void handle_stream_token(std::string_view token) override;
    void handle_final_result(const json& result) override;
    void handle_error(const std::string& error_message) override;
    std::string get_handler_type() const override;
    bool is_active() const override;
    
private:
    std::ostream& output_stream_;
    bool active_ = true;
    std::mutex output_mutex_;
    
    /**
     * @brief 格式化输出
     * @param content 内容
     * @param prefix 前缀（可选）
     */
    void format_output(const std::string& content, const std::string& prefix = "");
};

/**
 * @brief ImGui 处理器实现
 */
class ImGuiHandler : public UIHandler {
public:
    /**
     * @brief 构造函数
     * @param queue 线程安全的消息队列
     */
    explicit ImGuiHandler(std::shared_ptr<ThreadSafeQueue<StreamMessage>> queue);
    
    void handle_stream_token(std::string_view token) override;
    void handle_final_result(const json& result) override;
    void handle_error(const std::string& error_message) override;
    std::string get_handler_type() const override;
    bool is_active() const override;
    
private:
    std::shared_ptr<ThreadSafeQueue<StreamMessage>> queue_;
    bool active_ = true;
    
    /**
     * @brief 推送消息到队列
     * @param type 消息类型
     * @param content 消息内容
     */
    void push_message(const std::string& type, const std::string& content);
};

/**
 * @brief Web 处理器实现（SSE/WebSocket）
 */
class WebHandler : public UIHandler {
public:
    /**
     * @brief 构造函数
     * @param session_id 会话 ID
     * @param connection 连接信息
     */
    WebHandler(const std::string& session_id, 
              std::shared_ptr<WebConnectionInfo> connection);
    
    void handle_stream_token(std::string_view token) override;
    void handle_final_result(const json& result) override;
    void handle_error(const std::string& error_message) override;
    std::string get_handler_type() const override;
    bool is_active() const override;
    
    /**
     * @brief 发送 SSE 事件
     * @param event_type 事件类型
     * @param data 数据
     */
    void send_sse_event(const std::string& event_type, const std::string& data);
    
    /**
     * @brief 发送 WebSocket 消息
     * @param message 消息（JSON 格式）
     */
    void send_ws_message(const json& message);
    
private:
    std::string session_id_;
    std::shared_ptr<WebConnectionInfo> connection_;
    bool active_ = true;
    std::mutex connection_mutex_;
    
    /**
     * @brief 检查连接状态
     * @return true 如果连接有效
     */
    bool check_connection() const;
};

// ============================================================================
// UIManager 管理器
// ============================================================================

/**
 * @brief UIManager 管理器
 */
class UIManager {
public:
    /**
     * @brief 注册 CLI 输出处理器
     * @param handler CLI 处理器
     */
    void register_cli_handler(std::unique_ptr<CLIHandler> handler);
    
    /**
     * @brief 注册 ImGui 消息队列
     * @param handler ImGui 处理器
     */
    void register_gui_handler(std::unique_ptr<ImGuiHandler> handler);
    
    /**
     * @brief 注册 Web 连接（SSE/WebSocket）
     * @param session_id 会话 ID
     * @param handler Web 处理器
     */
    void register_web_connection(const std::string& session_id,
                                 std::unique_ptr<WebHandler> handler);
    
    /**
     * @brief 分发消息到所有注册的处理器
     * @param type 消息类型
     * @param data 数据（JSON 格式）
     */
    void dispatch_message(const std::string& type, const json& data);
    
    /**
     * @brief 流式输出（分发到所有处理器）
     * @param session_id 会话 ID
     * @param token token 内容
     */
    void stream_token(const std::string& session_id, std::string_view token);
    
    /**
     * @brief 移除处理器
     * @param handler_id 处理器 ID
     */
    void unregister_handler(const std::string& handler_id);
    
    /**
     * @brief 列出所有活跃的处理器
     * @return 处理器 ID 列表
     */
    std::vector<std::string> list_active_handlers() const;
    
private:
    std::vector<std::unique_ptr<UIHandler>> handlers_;
    std::map<std::string, std::unique_ptr<UIHandler>> session_handlers_;
    std::mutex handlers_mutex_;
    
    /**
     * @brief 分发到所有处理器
     * @param action 操作函数
     */
    void dispatch_to_all(const std::function<void(UIHandler&)>& action);
};

} // namespace agent_framework

#endif // __AGENT_UI_MANAGER_H__
