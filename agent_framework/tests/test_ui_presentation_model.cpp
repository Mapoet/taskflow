#include <agent/ui/presentation_model.hpp>

#include <cassert>
#include <string>

using namespace agent_framework;

int main() {
    UiPresentationModel model(4, 2, 1024);
    model.set_runtime_metadata("s", "openai", "model", "connected");
    model.begin_user_turn("你好");
    model.append_stream_token("轨道");
    model.append_stream_token("结果");
    model.append_thinking_token("可展示的推理摘要");
    auto s = model.snapshot();
    assert(s.run_state == UiRunState::Running);
    assert(s.turns.size() == 2U);
    assert(s.turns.back().content == "轨道结果");
    assert(s.turns.back().raw_markdown == "轨道结果");
    assert(s.turns.back().thinking_raw == "可展示的推理摘要");
    assert(s.turns.back().streaming);

    ToolExecutionEvent e{ToolExecutionPhase::Started, "fs_search", "c1", json{{"q", "x"}}, {}};
    model.observe_tool(e);
    e.phase = ToolExecutionPhase::Completed;
    e.result = json{{"ok", true}};
    model.observe_tool(e);
    s = model.snapshot();
    assert(s.tools.size() == 1U);
    assert(s.tools[0].state == UiRunState::Completed);

    model.complete(json{{"final_answer", "ignored because streamed"},
                        {"reasoning", "must remain private"}});
    s = model.snapshot();
    assert(s.run_state == UiRunState::Completed);
    assert(!s.turns.back().streaming);
    assert(s.turns.back().thinking_raw.find("must remain private") == std::string::npos);

    model.begin_user_turn("artifact");
    assert(model.observe_artifact(json{{"id", "plot-1"},
                                      {"mime", "image/png"},
                                      {"path", "plots/result.png"},
                                      {"caption", "Result plot"}}));
    assert(!model.observe_artifact(json{{"id", "escape"},
                                       {"mime", "image/png"},
                                       {"path", "../secret.png"}}));
    s = model.snapshot();
    assert(s.turns.back().attachments.size() == 1U);
    assert(s.turns.back().blocks.back().kind == UiContentBlockKind::Image);
    model.complete(json{{"final_answer", "artifact ready"},
                        {"displayable_reasoning", "Checked the generated plot."}});
    s = model.snapshot();
    assert(s.turns.back().thinking_raw == "Checked the generated plot.");

    model.begin_user_turn("fail");
    model.fail("network unavailable");
    s = model.snapshot();
    assert(s.run_state == UiRunState::Failed);
    assert(s.last_error == "network unavailable");
    assert(s.turns.back().error);

    model.begin_user_turn("cancel");
    model.cancel();
    assert(model.snapshot().run_state == UiRunState::Cancelled);

    model.add_system_notice("MCP unavailable: python_execute", true);
    s = model.snapshot();
    assert(s.turns.back().role == UiTurnRole::System);
    assert(s.turns.back().error);
    assert(s.turns.back().content.find("python_execute") != std::string::npos);

    model.load_demo_state();
    s = model.snapshot();
    assert(s.run_state == UiRunState::Completed);
    assert(s.tools.size() == 2U); // bounded to the newest two
    assert(s.turns.size() == 2U);
    return 0;
}
