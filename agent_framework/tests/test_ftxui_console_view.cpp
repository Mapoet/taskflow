#include "common/ftxui_console_view.hpp"

#include <agent/ui/presentation_model.hpp>
#include <ftxui/screen/string.hpp>

#include <cassert>
#include <string>

using namespace agent_framework;
using namespace agent_framework::example;

int main() {
    const auto cjk = wrap_terminal_text(
        "中文连续文本没有空格也必须按照终端显示列宽自动换行", 12);
    assert(cjk.size() > 1U);
    for (const auto& line : cjk) assert(ftxui::string_width(line) <= 12);
    std::string cjk_joined;
    for (const auto& line : cjk) cjk_joined += line;
    assert(cjk_joined == "中文连续文本没有空格也必须按照终端显示列宽自动换行");

    const auto mixed = wrap_terminal_text(
        "GNSS-R/RO路径/very/long/path/without/breaks 🛰️ ā", 10);
    assert(mixed.size() > 1U);
    for (const auto& line : mixed) assert(ftxui::string_width(line) <= 10);

    const auto explicit_lines = wrap_terminal_text("第一行\n第二行\n", 40);
    assert(explicit_lines.size() == 3U);
    assert(explicit_lines[0] == "第一行" && explicit_lines[1] == "第二行");

    UiPresentationModel model;
    model.load_demo_state();
    model.begin_user_turn("中文输入：分析 GNSS 掩星与电离层扰动 🛰️");
    model.append_stream_token("正在融合观测、模型与空间天气证据。");
    const auto snapshot = model.snapshot();

    FtxuiSkillStatus skills;
    skills.enabled = true;
    skills.count = 3;
    skills.generation = 7;
    skills.diagnostics = 2;
    skills.errors = 1;
    skills.root = "/workspace/.codex/skills";
    skills.active = "research-helper";

    const std::string wide = FtxuiConsoleView::render_for_test(snapshot, skills, true, 160, 42);
    assert(wide.find("SCIENTIFIC CONSOLE") != std::string::npos);
    assert(wide.find("CAPABILITIES") != std::string::npos);
    assert(wide.find("CONVERSATION") != std::string::npos);
    assert(wide.find("TOOL ACTIVITY") != std::string::npos);
    assert(wide.find("research-helper") != std::string::npos);
    assert(wide.find("errors=1") != std::string::npos);
    assert(wide.find("GNSS") != std::string::npos);
    assert(wide.find("RUNNING") != std::string::npos);
    assert(wide.find("wide") != std::string::npos);

    const std::string operations =
        FtxuiConsoleView::render_operations_for_test(snapshot, skills, 160, 96);
    assert(operations.find("PHASE 4 OPERATIONS") != std::string::npos);
    assert(operations.find("PLAN / EVIDENCE") != std::string::npos);
    assert(operations.find("MEMORY VIEW") != std::string::npos);
    assert(operations.find("LLM INVOCATIONS") != std::string::npos);
    assert(operations.find("FIVE-LAYER ASSURANCE") != std::string::npos);
    assert(operations.find("HITL") != std::string::npos);

    ui::NativeWorkbenchSnapshot workbench;
    workbench.selected_session_id = "session-orbital";
    workbench.sessions.push_back({"session-orbital", "conversation-orbital",
        "Orbital analysis and runtime closure", "Production certification", "active", 7, false});
    workbench.sessions.push_back({"session-second", "conversation-second",
        "第二个超长中文会话标题用于验证终端显示列宽自动折行", "Local workspace", "trashed", 3, false});
    workbench.subject.tenant_id = "local"; workbench.subject.organization_id = "local";
    workbench.subject.project_id = "taskflow"; workbench.subject.workspace_id = "agent-framework";
    workbench.subject.principal_id = "local-user"; workbench.subject.session_id = "session-orbital";
    workbench.subject.agent_id = "tui-agent"; workbench.subject.authorization_revision = 2;
    workbench.settings_revision = 4; workbench.settings_authorization_revision = 2;
    workbench.settings_fields = nlohmann::json::array({
        {{"key", "provider.model"}, {"category", "Provider / Model"}, {"label", "Model"},
         {"value", "deepseek-chat"}, {"mutability", "restart_required"}}
    });
    const std::string native = FtxuiConsoleView::render_workbench_for_test(
        snapshot, skills, workbench, 160, 48);
    assert(native.find("AGENT WORKBENCH") != std::string::npos);
    assert(native.find("SESSIONS") != std::string::npos);
    assert(native.find("Orbital analysis") != std::string::npos);
    assert(native.find("local-user") != std::string::npos);
    assert(native.find("Conversation") != std::string::npos);

    workbench.view = ui::NativeWorkbenchView::Settings;
    const std::string native_settings = FtxuiConsoleView::render_workbench_for_test(
        snapshot, skills, workbench, 120, 42);
    assert(native_settings.find("SYSTEM SETTINGS") != std::string::npos);
    assert(native_settings.find("Provider / Model") != std::string::npos);
    assert(native_settings.find("deepseek-chat") != std::string::npos);

    const std::string medium = FtxuiConsoleView::render_for_test(snapshot, skills, false, 100, 30);
    assert(medium.find("CAPABILITIES") != std::string::npos);
    assert(medium.find("CONVERSATION") != std::string::npos);
    assert(medium.find("medium") != std::string::npos);
    assert(medium.find("READY") != std::string::npos);

    const std::string compact = FtxuiConsoleView::render_for_test(snapshot, skills, false, 70, 24);
    assert(compact.find("CONVERSATION") != std::string::npos);
    assert(compact.find("compact") != std::string::npos);
    assert(compact.find("[1] capabilities") != std::string::npos);
    assert(compact.find("GNSS") != std::string::npos);

    UiPresentationModel failed_model;
    failed_model.load_demo_state();
    failed_model.fail("MCP resource injection failed safely");
    const std::string failed = FtxuiConsoleView::render_for_test(
        failed_model.snapshot(), skills, false, 90, 30);
    assert(failed.find("failed") != std::string::npos);
    assert(failed.find("MCP resource injection failed safely") != std::string::npos);
    return 0;
}
