/**
 * @file tui_handler.hpp
 * @brief WP2.U Track T：终端全屏 UI 的缓冲型 UIHandler（无 ncurses 依赖）
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_TUI_TUI_HANDLER_H__
#define __AGENT_TUI_TUI_HANDLER_H__

#include <agent/core/types.hpp>
#include <agent/ui/ui_manager.hpp>
#include <agent/ui/presentation_model.hpp>

#include <mutex>
#include <string>

namespace agent_framework {

/**
 * @brief 将流式/终稿/错误/aux 写入 UTF-8 有界缓冲；渲染由 tui_agent_demo 主循环负责
 */
class TuiHandler : public UIHandler {
public:
    explicit TuiHandler(std::shared_ptr<UiPresentationModel> presentation = {});

    void handle_stream_token(std::string_view token) override;
    void handle_stream_chunk(UiStreamChannel channel, std::string_view token) override;
    void handle_final_result(const json& result) override;
    void handle_error(const std::string& error_message) override;
    void handle_aux_event(std::string_view type, const json& payload) override;
    std::string get_handler_type() const override;
    bool is_active() const override;

    struct DisplaySnapshot {
        std::string stream;
        std::string aux;
    };

    DisplaySnapshot snapshot() const;
    UiPresentationSnapshot presentation_snapshot() const;

private:
    void append_capped(std::string& buf, std::string_view chunk, std::size_t max_bytes);

    mutable std::mutex mutex_;
    std::string stream_text_;
    std::string aux_text_;
    std::shared_ptr<UiPresentationModel> presentation_;
    bool active_ = true;
    static constexpr std::size_t k_max_stream_bytes = 64 * 1024;
    static constexpr std::size_t k_max_aux_bytes = 16 * 1024;
};

} // namespace agent_framework

#endif // __AGENT_TUI_TUI_HANDLER_H__
