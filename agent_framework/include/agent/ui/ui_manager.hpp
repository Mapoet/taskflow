/**
 * @file ui_manager.hpp
 * @brief UI Manager 模块：多客户端输出适配
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_UI_MANAGER_H__
#define __AGENT_UI_MANAGER_H__

#include <agent/core/types.hpp>
#include <agent/ui/thread_safe_queue.hpp>
#include <agent/ui/phase4_operations.hpp>

#include <atomic>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <mutex>
#include <deque>
#include <iostream>
#include <ostream>
#include <queue>

namespace agent_framework {

class UiPresentationModel;

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
     * @brief Typed stream channel. The compatibility default forwards answer chunks and
     *        deliberately ignores thinking chunks so legacy/CLI handlers never expose
     *        model-internal reasoning by accident.
     */
    virtual void handle_stream_chunk(UiStreamChannel channel, std::string_view token) {
        if (channel == UiStreamChannel::Answer) handle_stream_token(token);
    }

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

    /**
     * @brief WP2.U：辅助事件（tool_start / tool_end 等）；默认忽略
     */
    virtual void handle_aux_event(std::string_view /*type*/, const json& /*payload*/) {}
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
    void handle_aux_event(std::string_view type, const json& payload) override;
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
     * @param queue 线程安全的消息队列
     * @param session_id 会话 id（单用户 demo 默认 default）
     */
    explicit ImGuiHandler(std::shared_ptr<ThreadSafeQueue<StreamMessage>> queue,
                         std::string session_id = "default",
                         std::shared_ptr<UiPresentationModel> presentation = {});

    void handle_stream_token(std::string_view token) override;
    void handle_stream_chunk(UiStreamChannel channel, std::string_view token) override;
    void handle_final_result(const json& result) override;
    void handle_error(const std::string& error_message) override;
    void handle_aux_event(std::string_view type, const json& payload) override;
    std::string get_handler_type() const override;
    bool is_active() const override;

    /**
     * @brief 渲染线程每帧最多 drain max_n 条（WP2.U；默认 env AGENT_IMGUI_QUEUE_DRAIN_MAX=256）
     * @return 实际弹出条数
     */
    std::size_t drain_messages(std::vector<StreamMessage>& out, std::size_t max_n);

private:
    std::shared_ptr<ThreadSafeQueue<StreamMessage>> queue_;
    std::string session_id_;
    bool active_ = true;
    /** UTF-8 bytes pushed via handle_stream_token this turn (reset in handle_final_result). */
    std::atomic<std::size_t> streamed_utf8_bytes_{0};
    std::shared_ptr<UiPresentationModel> presentation_;

    void push_message(const std::string& type, const std::string& content,
                      UiStreamChannel channel = UiStreamChannel::Answer);
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
    void handle_stream_chunk(UiStreamChannel channel, std::string_view token) override;
    void handle_final_result(const json& result) override;
    void handle_error(const std::string& error_message) override;
    void handle_aux_event(std::string_view type, const json& payload) override;
    std::string get_handler_type() const override;
    bool is_active() const override;

    /**
     * @brief WP2.U Web demo：取一条已格式化的 SSE 块（含 "data: ...\\n\\n"），无则 false
     */
    bool try_pop_sse_chunk(std::string& out);

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
    std::mutex sse_mutex_;
    std::deque<std::string> sse_chunks_;

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
     * @brief 注册任意 UIHandler（测试 / TUI 等；与 register_cli_handler 等价入队）
     */
    void register_handler(std::unique_ptr<UIHandler> handler);

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

    /** Publish the canonical display-safe Phase 4 control-plane projection. */
    void publish_phase4_operations(const Phase4OperationsSnapshot& snapshot);

    /**
     * @brief WP2.U：终稿 JSON 分发到全部 handler（与 Sink 回调对齐）
     */
    void dispatch_final_result(const json& result);

    /**
     * @brief WP2.U：错误分发到全部 handler
     */
    void dispatch_error(const std::string& error_message);

    /**
     * @brief 流式输出（分发到所有处理器）
     * @param session_id 会话 ID
     * @param token token 内容
     */
    void stream_token(const std::string& session_id, std::string_view token);

    /** @brief Stream an explicitly displayable thinking summary. */
    void stream_thinking(const std::string& session_id, std::string_view token);

    /** @brief Typed stream primitive used by answer/thinking compatibility wrappers. */
    void stream_chunk(const std::string& session_id, UiStreamChannel channel,
                      std::string_view token);

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
    mutable std::mutex handlers_mutex_;

    /**
     * @brief 分发到所有处理器
     * @param action 操作函数
     */
    void dispatch_to_all(const std::function<void(UIHandler&)>& action);
};

} // namespace agent_framework

#endif // __AGENT_UI_MANAGER_H__
