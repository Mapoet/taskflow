#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/api/v1/runtime_settings_api.hpp"
#include "agent/api/v1/session_run_api.hpp"
#include "agent/identity/runtime_subject.hpp"

namespace agent_framework::ui {

enum class NativeWorkbenchView {
    Conversation, Understanding, Plan, Memory, Files, Approval, Evidence, Profile, Settings,
};

struct NativeSessionItem {
    std::string session_id;
    std::string conversation_id;
    std::string title;
    std::string folder;
    std::string state;
    std::uint64_t revision{0};
    bool pinned{false};
};

struct NativeWorkbenchSnapshot {
    std::vector<NativeSessionItem> sessions;
    std::string selected_session_id;
    NativeWorkbenchView view{NativeWorkbenchView::Conversation};
    identity::RuntimeSubject subject;
    std::uint64_t settings_revision{0};
    std::uint64_t settings_authorization_revision{0};
    nlohmann::json settings_fields = nlohmann::json::array();
    std::string status;
    std::string error;
};

/** Renderer-neutral, revision-aware controller shared by TUI and ImGui. */
class NativeWorkbenchController {
public:
    NativeWorkbenchController(api::v1::SessionRunApi&, api::v1::RuntimeSettingsStore&,
                              identity::RuntimeSubject);
    bool refresh();
    NativeWorkbenchSnapshot snapshot() const;
    bool select_session(std::string_view session_id);
    bool set_view(NativeWorkbenchView view);
    bool create_session(std::string title);
    bool rename_selected(std::string title);
    bool trash_selected();
    bool restore_selected();
    bool request_purge_selected();
    bool confirm_purge_selected();
    bool update_setting(std::string_view key, const nlohmann::json& value);
    static std::string_view name(NativeWorkbenchView view) noexcept;

private:
    bool accept(const api::v1::ApiResult&, std::string success_status);
    const NativeSessionItem* selected_locked() const;
    api::v1::SessionRunApi& sessions_;
    api::v1::RuntimeSettingsStore& settings_;
    mutable std::mutex mutex_;
    NativeWorkbenchSnapshot state_;
};

}  // namespace agent_framework::ui
