/**
 * @file tool_side_effect.cpp
 * @brief ToolSideEffect string parsing
 */

#include "agent/types.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace agent_framework {
namespace {

void ascii_lower_inplace(std::string& s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
}

} // namespace

std::optional<ToolSideEffect> tool_side_effect_from_string(std::string_view s) {
    std::string t(s);
    ascii_lower_inplace(t);
    if (t.empty()) {
        return std::nullopt;
    }
    if (t == "readonly" || t == "read_only" || t == "read" || t == "r") {
        return ToolSideEffect::ReadOnly;
    }
    if (t == "write" || t == "w" || t == "mutate") {
        return ToolSideEffect::Write;
    }
    if (t == "unknown" || t == "u" || t == "auto") {
        return ToolSideEffect::Unknown;
    }
    return std::nullopt;
}

} // namespace agent_framework
