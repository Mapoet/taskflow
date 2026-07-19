#include "imgui_text_input_support.hpp"

#include <clocale>

namespace agent_framework::example {
namespace {

std::string current_ctype() {
    const char* value = std::setlocale(LC_CTYPE, nullptr);
    return value ? value : std::string{};
}

bool is_c_locale(const std::string& value) {
    return value.empty() || value == "C" || value == "POSIX";
}

} // namespace

ImGuiTextInputLocaleStatus initialize_imgui_text_input_locale() {
    ImGuiTextInputLocaleStatus status;
    status.before = current_ctype();
    if (is_c_locale(status.before)) {
        status.environment_applied = std::setlocale(LC_CTYPE, "") != nullptr;
    }
    status.after = current_ctype();
    status.unicode_ready = !is_c_locale(status.after);
    return status;
}

} // namespace agent_framework::example
