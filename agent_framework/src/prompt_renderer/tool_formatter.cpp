/**
 * @file tool_formatter.cpp
 * @brief 工具格式化器实现
 */

#include "agent/prompt_renderer.hpp"

namespace agent_framework {

json OpenAIToolFormatter::convert_to_openai_format(const ToolMeta& tool) {
    json params = tool.schema.is_null() || tool.schema.empty() ? json::object() : tool.schema;
    json fn =
        json{{"name", tool.name}, {"description", tool.description}, {"parameters", std::move(params)}};
    return json{{"type", "function"}, {"function", std::move(fn)}};
}

json OpenAIToolFormatter::format_tools(const std::vector<ToolMeta>& tools) {
    json arr = json::array();
    for (const auto& t : tools) {
        arr.push_back(convert_to_openai_format(t));
    }
    return arr;
}

std::string OpenAIToolFormatter::format_tools_as_text(const std::vector<ToolMeta>& tools) {
    std::ostringstream o;
    for (const auto& t : tools) {
        o << t.name << ": " << t.description << '\n';
    }
    return o.str();
}

std::vector<std::string> OpenAIToolFormatter::supported_models() const {
    return {"gpt-*", "openai-*"};
}

json AnthropicToolFormatter::convert_to_anthropic_format(const ToolMeta& tool) {
    json schema = tool.schema.is_null() || tool.schema.empty() ? json::object() : tool.schema;
    return json{{"name", tool.name}, {"description", tool.description}, {"input_schema", std::move(schema)}};
}

json AnthropicToolFormatter::format_tools(const std::vector<ToolMeta>& tools) {
    json arr = json::array();
    for (const auto& t : tools) {
        arr.push_back(convert_to_anthropic_format(t));
    }
    return arr;
}

std::string AnthropicToolFormatter::format_tools_as_text(const std::vector<ToolMeta>& tools) {
    return OpenAIToolFormatter().format_tools_as_text(tools);
}

std::vector<std::string> AnthropicToolFormatter::supported_models() const {
    return {"claude-*", "anthropic-*"};
}

json GeminiToolFormatter::convert_to_gemini_format(const ToolMeta& tool) {
    json decl = json{{"name", tool.name}, {"description", tool.description}};
    decl["parameters"] = tool.schema.is_null() ? json::object() : tool.schema;
    return json{{"function_declarations", json::array({decl})}};
}

json GeminiToolFormatter::format_tools(const std::vector<ToolMeta>& tools) {
    json decls = json::array();
    for (const auto& t : tools) {
        json d = json{{"name", t.name}, {"description", t.description}};
        d["parameters"] = t.schema.is_null() ? json::object() : t.schema;
        decls.push_back(std::move(d));
    }
    return json{{"function_declarations", std::move(decls)}};
}

std::string GeminiToolFormatter::format_tools_as_text(const std::vector<ToolMeta>& tools) {
    return OpenAIToolFormatter().format_tools_as_text(tools);
}

std::vector<std::string> GeminiToolFormatter::supported_models() const {
    return {"gemini-*"};
}

} // namespace agent_framework

