#pragma once

#include <string>

namespace agent_framework::example {

struct ImGuiTextInputLocaleStatus {
    std::string before;
    std::string after;
    bool environment_applied = false;
    bool unicode_ready = false;
};

// X11 XIM requires a non-C LC_CTYPE before GLFW initializes its input method.
ImGuiTextInputLocaleStatus initialize_imgui_text_input_locale();

} // namespace agent_framework::example
