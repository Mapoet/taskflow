#ifndef AGENT_EXAMPLES_IMGUI_CONSOLE_VIEW_HPP
#define AGENT_EXAMPLES_IMGUI_CONSOLE_VIEW_HPP

#include <agent/ui/presentation_model.hpp>
#include <agent/ui/native_workbench.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>

namespace agent_framework::example {

struct ImGuiConsoleAction {
    bool send = false;
    bool cancel = false;
    bool quit = false;
    std::string prompt;
};

struct ImGuiSkillStatus {
    bool enabled = false;
    std::size_t count = 0;
    std::uint64_t generation = 0;
    std::size_t diagnostics = 0;
    std::size_t errors = 0;
    std::string root;
    std::string active = "-";
};

void apply_scientific_console_theme();
/** Release OpenGL textures created for sandboxed image artifacts. Call before GL teardown. */
void clear_imgui_artifact_textures();
ImGuiConsoleAction render_scientific_console(const UiPresentationSnapshot& snapshot,
                                             const ImGuiSkillStatus& skills,
                                             char* input, std::size_t input_size,
                                             bool agent_busy,
                                             const std::shared_ptr<ui::NativeWorkbenchController>& workbench = {});

} // namespace agent_framework::example

#endif
