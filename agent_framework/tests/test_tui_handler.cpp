#include <agent/ui/presentation_model.hpp>
#include <agent/ui/tui_handler.hpp>

#include <cassert>
#include <memory>
#include <string>

using namespace agent_framework;

int main() {
    auto presentation = std::make_shared<UiPresentationModel>();
    presentation->begin_user_turn("inspect the latest observation");
    TuiHandler handler(presentation);

    handler.handle_stream_token("Analysis ");
    handler.handle_stream_token("complete.");
    handler.handle_aux_event("tool_start", json{{"name", "filesystem"}});
    handler.handle_final_result(json{{"final_answer", "Analysis complete."}, {"iteration", 1}});

    const auto display = handler.snapshot();
    assert(display.stream.find("Analysis complete.") != std::string::npos);
    assert(display.aux.find("tool_start") != std::string::npos);

    const auto state = handler.presentation_snapshot();
    assert(state.run_state == UiRunState::Completed);
    assert(state.turns.size() == 2);
    assert(state.turns.back().content == "Analysis complete.");
    assert(!state.turns.back().streaming);

    handler.handle_error("network unavailable");
    const auto failed = handler.presentation_snapshot();
    assert(failed.run_state == UiRunState::Failed);
    assert(failed.last_error == "network unavailable");
    return 0;
}
