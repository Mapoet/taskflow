#pragma once

#include <agent/api/v1/runtime_settings_api.hpp>
#include <agent/api/v1/session_run_api.hpp>
#include <agent/session/run_supervisor.hpp>
#include <agent/session/session_catalog.hpp>
#include <agent/ui/native_workbench.hpp>

#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace agent_framework::example {

struct NativeWorkbenchBootstrapOptions {
    std::string state_directory;
    std::string agent_name;
    std::string provider;
    std::string model;
    std::string skills_root;
    std::string mcp_registry;
    std::string sandbox_root;
    bool mcp_enabled{true};
    bool skills_enabled{true};
};

struct NativeWorkbenchRuntime {
    std::shared_ptr<session::SQLiteSessionCatalog> catalog;
    std::shared_ptr<session::SQLiteSessionRunSupervisor> supervisor;
    std::shared_ptr<api::v1::SessionRunApi> sessions;
    std::shared_ptr<api::v1::RuntimeSettingsStore> settings;
    std::shared_ptr<ui::NativeWorkbenchController> controller;
    identity::RuntimeSubject subject;
    std::string state_directory;
};

inline bool apply_native_initial_view(
    const std::shared_ptr<ui::NativeWorkbenchController>& controller,
    std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const std::pair<std::string_view, ui::NativeWorkbenchView> views[] = {
        {"conversation", ui::NativeWorkbenchView::Conversation},
        {"understanding", ui::NativeWorkbenchView::Understanding},
        {"plan", ui::NativeWorkbenchView::Plan},
        {"memory", ui::NativeWorkbenchView::Memory},
        {"files", ui::NativeWorkbenchView::Files},
        {"approval", ui::NativeWorkbenchView::Approval},
        {"evidence", ui::NativeWorkbenchView::Evidence},
        {"operations", ui::NativeWorkbenchView::Evidence},
        {"profile", ui::NativeWorkbenchView::Profile},
        {"settings", ui::NativeWorkbenchView::Settings},
    };
    const auto found = std::find_if(std::begin(views), std::end(views),
        [&](const auto& item) { return item.first == value; });
    return found != std::end(views) && controller && controller->set_view(found->second);
}

inline NativeWorkbenchRuntime build_native_workbench(
    NativeWorkbenchBootstrapOptions options) {
    NativeWorkbenchRuntime result;
    if (options.state_directory.empty()) {
        const char* configured = std::getenv("AGENT_NATIVE_UI_STATE_DIR");
        options.state_directory = configured && *configured
            ? configured : (std::filesystem::path(".agent-framework") / "native-workbench").string();
    }
    std::filesystem::create_directories(options.state_directory);
    result.state_directory = options.state_directory;
    const auto database =
        (std::filesystem::path(options.state_directory) / "workbench.sqlite3").string();
    result.catalog = std::make_shared<session::SQLiteSessionCatalog>(database);
    result.supervisor = std::make_shared<session::SQLiteSessionRunSupervisor>(database);
    result.sessions = std::make_shared<api::v1::SessionRunApi>(
        *result.catalog, *result.supervisor);

    result.subject.tenant_id = "local";
    result.subject.organization_id = "local";
    result.subject.project_id = "taskflow";
    result.subject.workspace_id = "agent-framework";
    result.subject.principal_id = "local-user";
    result.subject.session_id = "orbital-analysis";
    result.subject.conversation_id = "orbital-analysis";
    result.subject.agent_id = options.agent_name.empty() ? "native-ui" : options.agent_name;
    result.subject.authorization_revision = 1;
    result.subject.authenticated = true;

    session::ProductSession initial;
    initial.tenant_id = result.subject.tenant_id;
    initial.organization_id = result.subject.organization_id;
    initial.project_id = result.subject.project_id;
    initial.workspace_id = result.subject.workspace_id;
    initial.session_id = result.subject.session_id;
    initial.conversation_id = result.subject.conversation_id;
    initial.owner_principal_id = result.subject.principal_id;
    initial.title = "Orbital analysis and runtime closure";
    initial.folder = "Production certification";
    initial.tags = {"native", "harness"};
    const auto created = result.sessions->create_session(result.subject, initial);
    if (!created.ok())
        throw std::runtime_error("native_default_session_failed:" +
                                 created.body.value("error", "unknown"));

    const char* api_key = std::getenv("AGENT_LLM_API_KEY");
    if (!api_key || !*api_key) api_key = std::getenv("OPENAI_API_KEY");
    nlohmann::json defaults = {
        {"provider.id", options.provider.empty() ? "default" : options.provider},
        {"provider.model", options.model}, {"provider.endpoint", ""},
        {"provider.credentials_configured", api_key && *api_key},
        {"mcp.enabled", options.mcp_enabled}, {"mcp.registry", options.mcp_registry},
        {"skills.enabled", options.skills_enabled}, {"skills.root", options.skills_root},
        {"sandbox.mode", "workspace"}, {"sandbox.root", options.sandbox_root},
        {"workspace.directory", std::filesystem::current_path().string()},
        {"planning.depth", "comprehensive"}, {"memory.strategy", "adaptive"},
        {"assurance.tier", "professional"}, {"judge.mode", "required"},
        {"logging.level", "info"}, {"logging.redaction", true},
        {"observability.enabled", true}, {"appearance.theme", "dark"},
        {"appearance.language", "zh-CN"},
    };
    result.settings = std::make_shared<api::v1::RuntimeSettingsStore>(
        (std::filesystem::path(options.state_directory) / "runtime-settings.sqlite3").string(),
        std::move(defaults));
    result.controller = std::make_shared<ui::NativeWorkbenchController>(
        *result.sessions, *result.settings, result.subject);
    return result;
}

}  // namespace agent_framework::example
