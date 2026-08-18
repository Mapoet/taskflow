#include <agent/api/v1/runtime_settings_api.hpp>
#include <agent/api/v1/session_run_api.hpp>
#include <agent/session/session_catalog.hpp>
#include <agent/session/run_supervisor.hpp>
#include <agent/ui/native_workbench.hpp>

#include <cassert>
#include <algorithm>
#include <chrono>
#include <filesystem>

using namespace agent_framework;

int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("af-native-workbench-" + std::to_string(nonce));
    std::filesystem::create_directories(root);
    const auto workbench_db = (root / "workbench.sqlite3").string();
    session::SQLiteSessionCatalog catalog(workbench_db);
    session::SQLiteSessionRunSupervisor supervisor(workbench_db);
    api::v1::SessionRunApi sessions(catalog, supervisor);
    api::v1::RuntimeSettingsStore settings((root / "settings.sqlite3").string(),
        {{"provider.id", "openai"}, {"appearance.language", "zh-CN"},
         {"provider.credentials_configured", false}});

    identity::RuntimeSubject subject;
    subject.tenant_id = "local"; subject.organization_id = "local";
    subject.project_id = "taskflow"; subject.workspace_id = "agent-framework";
    subject.principal_id = "local-user"; subject.session_id = "native-default";
    subject.conversation_id = "native-default"; subject.agent_id = "native-test";
    subject.authorization_revision = 1; subject.authenticated = true;

    session::ProductSession seed;
    seed.tenant_id = subject.tenant_id; seed.organization_id = subject.organization_id;
    seed.project_id = subject.project_id; seed.workspace_id = subject.workspace_id;
    seed.session_id = subject.session_id; seed.conversation_id = subject.conversation_id;
    seed.owner_principal_id = subject.principal_id; seed.title = "Native default";
    seed.folder = "Local workspace";
    assert(sessions.create_session(subject, seed).ok());

    ui::NativeWorkbenchController controller(sessions, settings, subject);
    auto snapshot = controller.snapshot();
    assert(snapshot.sessions.size() == 1U);
    assert(snapshot.settings_fields.size() == 20U);
    assert(snapshot.subject.principal_id == "local-user");
    assert(controller.set_view(ui::NativeWorkbenchView::Settings));
    assert(controller.snapshot().view == ui::NativeWorkbenchView::Settings);

    assert(controller.create_session("Native managed Session"));
    snapshot = controller.snapshot();
    assert(snapshot.sessions.size() == 2U);
    assert(snapshot.selected_session_id.find("session-native-") == 0);
    assert(controller.rename_selected("Renamed Session"));
    snapshot = controller.snapshot();
    const auto selected = std::find_if(snapshot.sessions.begin(), snapshot.sessions.end(),
        [&](const auto& item) { return item.session_id == snapshot.selected_session_id; });
    assert(selected != snapshot.sessions.end() && selected->title == "Renamed Session");
    assert(controller.trash_selected());
    assert(controller.restore_selected());
    assert(controller.trash_selected());
    assert(controller.request_purge_selected());
    assert(controller.confirm_purge_selected());
    snapshot = controller.snapshot();
    assert(snapshot.sessions.size() == 1U);

    assert(controller.update_setting("appearance.language", "en"));
    snapshot = controller.snapshot();
    assert(snapshot.settings_revision == 2U);
    assert(snapshot.status == "Setting applied");
    assert(!controller.update_setting("provider.credentials_configured", true));
    assert(controller.snapshot().error == "setting_is_read_only");

    std::filesystem::remove_all(root);
    return 0;
}
