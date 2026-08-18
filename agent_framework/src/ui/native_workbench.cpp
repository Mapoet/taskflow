#include "agent/ui/native_workbench.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <utility>

namespace agent_framework::ui {
namespace {
using nlohmann::json;

std::string error_text(const api::v1::ApiResult& result) {
    if (result.body.contains("error") && result.body["error"].is_string())
        return result.body["error"].get<std::string>();
    return "native_workbench_request_failed:" + std::to_string(result.status);
}

std::string next_session_id() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "session-native-" + std::to_string(now) + "-" +
           std::to_string(sequence.fetch_add(1));
}
}  // namespace

NativeWorkbenchController::NativeWorkbenchController(
    api::v1::SessionRunApi& sessions, api::v1::RuntimeSettingsStore& settings,
    identity::RuntimeSubject subject)
    : sessions_(sessions), settings_(settings) {
    state_.subject = std::move(subject);
    state_.selected_session_id = state_.subject.session_id;
    (void)refresh();
}

std::string_view NativeWorkbenchController::name(NativeWorkbenchView view) noexcept {
    switch (view) {
        case NativeWorkbenchView::Conversation: return "Conversation";
        case NativeWorkbenchView::Understanding: return "Understanding";
        case NativeWorkbenchView::Plan: return "Plan";
        case NativeWorkbenchView::Memory: return "Memory";
        case NativeWorkbenchView::Files: return "Files";
        case NativeWorkbenchView::Approval: return "Approval";
        case NativeWorkbenchView::Evidence: return "Evidence";
        case NativeWorkbenchView::Profile: return "Profile";
        case NativeWorkbenchView::Settings: return "Settings";
    }
    return "Conversation";
}

bool NativeWorkbenchController::refresh() {
    identity::RuntimeSubject subject;
    {
        std::lock_guard lock(mutex_);
        subject = state_.subject;
    }
    session::SessionListQuery query;
    query.state.reset();
    query.limit = 100;
    const auto sessions = sessions_.list_sessions(subject, query);
    const auto settings = settings_.snapshot(subject);
    std::lock_guard lock(mutex_);
    if (!sessions.ok()) {
        state_.error = error_text(sessions);
        return false;
    }
    std::vector<NativeSessionItem> parsed;
    for (const auto& item : sessions.body.value("items", json::array())) {
        if (item.value("state", "") == "purged") continue;
        parsed.push_back({item.value("session_id", ""), item.value("conversation_id", ""),
                          item.value("title", ""), item.value("folder", ""),
                          item.value("state", "unknown"), item.value("revision", 0ULL),
                          item.value("pinned", false)});
    }
    state_.sessions = std::move(parsed);
    if (std::none_of(state_.sessions.begin(), state_.sessions.end(), [&](const auto& item) {
            return item.session_id == state_.selected_session_id;
        }) && !state_.sessions.empty()) {
        state_.selected_session_id = state_.sessions.front().session_id;
    }
    if (const auto* selected = selected_locked()) {
        state_.subject.session_id = selected->session_id;
        state_.subject.conversation_id = selected->conversation_id;
    }
    if (!settings.ok()) {
        state_.error = error_text(settings);
        return false;
    }
    state_.settings_revision = settings.body.value("revision", 0ULL);
    state_.settings_authorization_revision = settings.body.value("authorization_revision", 0ULL);
    state_.settings_fields = settings.body.value("fields", json::array());
    state_.error.clear();
    return true;
}

NativeWorkbenchSnapshot NativeWorkbenchController::snapshot() const {
    std::lock_guard lock(mutex_);
    return state_;
}

const NativeSessionItem* NativeWorkbenchController::selected_locked() const {
    const auto found = std::find_if(state_.sessions.begin(), state_.sessions.end(),
        [&](const auto& item) { return item.session_id == state_.selected_session_id; });
    return found == state_.sessions.end() ? nullptr : &*found;
}

bool NativeWorkbenchController::select_session(std::string_view session_id) {
    std::lock_guard lock(mutex_);
    const auto found = std::find_if(state_.sessions.begin(), state_.sessions.end(),
        [&](const auto& item) { return item.session_id == session_id; });
    if (found == state_.sessions.end()) {
        state_.error = "session_not_found";
        return false;
    }
    state_.selected_session_id = found->session_id;
    state_.subject.session_id = found->session_id;
    state_.subject.conversation_id = found->conversation_id;
    state_.view = NativeWorkbenchView::Conversation;
    state_.status = "Session selected · r" + std::to_string(found->revision);
    state_.error.clear();
    return true;
}

bool NativeWorkbenchController::set_view(NativeWorkbenchView view) {
    std::lock_guard lock(mutex_);
    state_.view = view;
    return true;
}

bool NativeWorkbenchController::accept(const api::v1::ApiResult& result,
                                       std::string success_status) {
    {
        std::lock_guard lock(mutex_);
        if (!result.ok()) {
            state_.error = error_text(result);
            if (result.status == 409 && result.body.contains("revision"))
                state_.error += " · authoritative r" +
                                std::to_string(result.body["revision"].get<std::uint64_t>());
            return false;
        }
        state_.status = std::move(success_status);
        state_.error.clear();
    }
    return refresh();
}

bool NativeWorkbenchController::create_session(std::string title) {
    identity::RuntimeSubject subject;
    {
        std::lock_guard lock(mutex_);
        subject = state_.subject;
    }
    const std::string id = next_session_id();
    session::ProductSession value;
    value.tenant_id = subject.tenant_id;
    value.organization_id = subject.organization_id;
    value.project_id = subject.project_id;
    value.workspace_id = subject.workspace_id;
    value.session_id = id;
    value.conversation_id = id;
    value.owner_principal_id = subject.principal_id;
    value.title = title.empty() ? "New Session" : std::move(title);
    value.folder = "Local workspace";
    const auto result = sessions_.create_session(subject, std::move(value));
    if (!accept(result, "Session created")) return false;
    return select_session(id);
}

bool NativeWorkbenchController::rename_selected(std::string title) {
    identity::RuntimeSubject subject; NativeSessionItem selected;
    {
        std::lock_guard lock(mutex_); subject = state_.subject;
        const auto* item = selected_locked(); if (!item) return false; selected = *item;
    }
    return accept(sessions_.rename_session(subject, selected.session_id, selected.revision, title),
                  "Session renamed");
}

bool NativeWorkbenchController::trash_selected() {
    identity::RuntimeSubject subject; NativeSessionItem selected;
    {
        std::lock_guard lock(mutex_); subject = state_.subject;
        const auto* item = selected_locked(); if (!item) return false; selected = *item;
    }
    return accept(sessions_.transition_session(subject, selected.session_id, selected.revision,
                                                session::ProductSessionState::Trashed),
                  "Session moved to trash");
}

bool NativeWorkbenchController::restore_selected() {
    identity::RuntimeSubject subject; NativeSessionItem selected;
    {
        std::lock_guard lock(mutex_); subject = state_.subject;
        const auto* item = selected_locked(); if (!item) return false; selected = *item;
    }
    return accept(sessions_.restore_session(subject, selected.session_id, selected.revision),
                  "Session restored");
}

bool NativeWorkbenchController::request_purge_selected() {
    identity::RuntimeSubject subject; NativeSessionItem selected;
    {
        std::lock_guard lock(mutex_); subject = state_.subject;
        const auto* item = selected_locked(); if (!item) return false; selected = *item;
    }
    return accept(sessions_.purge_session(subject, selected.session_id, selected.revision, false),
                  "Permanent purge confirmation required");
}

bool NativeWorkbenchController::confirm_purge_selected() {
    identity::RuntimeSubject subject; NativeSessionItem selected;
    {
        std::lock_guard lock(mutex_); subject = state_.subject;
        const auto* item = selected_locked(); if (!item) return false; selected = *item;
    }
    return accept(sessions_.purge_session(subject, selected.session_id, selected.revision, true),
                  "Session permanently purged");
}

bool NativeWorkbenchController::update_setting(std::string_view key, const json& value) {
    identity::RuntimeSubject subject; std::uint64_t revision = 0;
    {
        std::lock_guard lock(mutex_); subject = state_.subject; revision = state_.settings_revision;
    }
    const auto result = settings_.update(subject, revision, json{{std::string(key), value}});
    return accept(result, result.body.value("restart_required", false)
                              ? "Setting saved · runtime restart required" : "Setting applied");
}

}  // namespace agent_framework::ui
