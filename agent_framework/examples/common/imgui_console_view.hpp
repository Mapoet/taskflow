#ifndef AGENT_EXAMPLES_IMGUI_CONSOLE_VIEW_HPP
#define AGENT_EXAMPLES_IMGUI_CONSOLE_VIEW_HPP

#include <agent/ui/presentation_model.hpp>

#include <cstddef>
#include <string>

namespace agent_framework::example {

struct ImGuiConsoleAction {
    bool send = false;
    bool cancel = false;
    bool quit = false;
    std::string prompt;
};

void apply_scientific_console_theme();
ImGuiConsoleAction render_scientific_console(const UiPresentationSnapshot& snapshot,
                                             char* input, std::size_t input_size,
                                             bool agent_busy);

} // namespace agent_framework::example

#endif
