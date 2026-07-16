/**
 * @file user_input_types.hpp
 * @brief WP2.7 用户输入预处理数据结构（无 ToolBus 依赖）
 */
#ifndef __AGENT_USER_INPUT_TYPES_HPP__
#define __AGENT_USER_INPUT_TYPES_HPP__

#include <agent/core/types.hpp>

#include <optional>
#include <string>
#include <vector>

namespace agent_framework {

struct InjectedContextBlock {
    std::string source_kind;
    std::string source_ref;
    std::optional<std::string> mime_hint;
    std::string text_utf8;
    std::size_t byte_length = 0;
};

struct ControlAction {
    std::string command;
    json args = json::object();
    std::string raw_line;
};

struct ProcessedUserInput {
    std::string llm_user_text;
    std::vector<InjectedContextBlock> injected_context;
    std::vector<ControlAction> control_actions;
    std::vector<std::string> tier_a_violations;
};

} // namespace agent_framework

#endif
