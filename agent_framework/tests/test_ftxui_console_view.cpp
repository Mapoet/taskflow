#include "common/ftxui_console_view.hpp"

#include <agent/ui/presentation_model.hpp>

#include <cassert>
#include <string>

using namespace agent_framework;
using namespace agent_framework::example;

int main() {
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
