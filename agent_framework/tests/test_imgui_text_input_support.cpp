#include "../examples/common/imgui_text_input_support.hpp"

#include <cassert>
#include <clocale>
#include <iostream>

int main() {
    using agent_framework::example::initialize_imgui_text_input_locale;

    assert(std::setlocale(LC_CTYPE, "C") != nullptr);
    const auto status = initialize_imgui_text_input_locale();
    assert(status.before == "C");
    assert(status.environment_applied);
    assert(status.unicode_ready);
    assert(status.after != "C");
    assert(status.after != "POSIX");
    std::cout << "LC_CTYPE " << status.before << " -> " << status.after << '\n';
    return 0;
}
