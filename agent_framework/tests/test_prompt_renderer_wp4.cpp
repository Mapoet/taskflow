/**
 * @file test_prompt_renderer_wp4.cpp
 * @brief WP1.4 PromptRenderer 单测（无第三方框架）
 */

#include <agent/prompt_renderer.hpp>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;
using namespace agent_framework;

bool should_debug_print() {
    const char* e = std::getenv("AGENT_TEST_PROMPT_RENDERER_DEBUG");
    return e != nullptr && e[0] != '\0' && std::string(e) != "0";
}

void print_input_output(const char* title, const LLMInput& in, const RenderedPrompt& out) {
    if (!should_debug_print()) {
        return;
    }
    std::cout << "\n=== " << title << " ===\n";
    std::cout << "[input]\n";
    std::cout << "system_prompt:\n" << in.system_prompt << "\n";
    std::cout << "user_prompt:\n" << in.user_prompt << "\n";
    std::cout << "context:\n" << in.context << "\n";
    std::cout << "tools.count: " << in.tools.size() << "\n";
    std::cout << "history.count: " << in.history.size() << "\n";
    std::cout << "has_image: " << (in.image_data.has_value() ? "true" : "false") << "\n";
    std::cout << "has_audio: " << (in.audio_data.has_value() ? "true" : "false") << "\n";

    std::cout << "\n[output]\n";
    std::cout << "rendered_text:\n" << out.rendered_text << "\n";
    std::cout << "messages:\n" << json(out.messages).dump(2) << "\n";
    std::cout << "tools_json:\n" << out.tools_json.dump(2) << "\n";
    std::cout << "total_tokens: " << out.total_tokens << "\n";
}

void test_empty_history_no_tools() {
    PromptRenderer r;
    r.set_max_history_messages(20);
    r.register_tool_formatter("gpt-*", std::make_shared<OpenAIToolFormatter>());

    LLMInput in;
    in.system_prompt = "sys";
    in.user_prompt = "hi";
    in.context = "";
    in.tools = {};
    in.history = {};

    RenderedPrompt out = r.render(in, "gpt-4o");
    print_input_output("empty_history_no_tools", in, out);
    assert(out.messages.size() == 2U);
    assert(out.messages[0].at("role") == "system");
    assert(out.messages[1].at("role") == "user");
    assert(out.messages[1].at("content") == "hi");
    assert(out.tools_json.is_array());
    assert(out.tools_json.size() == 0);
}

void test_context_injected_into_system() {
    PromptRenderer r;
    r.register_tool_formatter("gpt-*", std::make_shared<OpenAIToolFormatter>());
    LLMInput in;
    in.system_prompt = "sys";
    in.user_prompt = "q";
    in.context = "ctx";
    RenderedPrompt out = r.render(in, "gpt-4o");
    print_input_output("context_injected_into_system", in, out);
    const std::string sys = out.messages[0].at("content").get<std::string>();
    assert(sys.find("sys") != std::string::npos);
    assert(sys.find("Retrieved context") != std::string::npos);
    assert(sys.find("ctx") != std::string::npos);
    assert(out.messages.back().at("content") == "q");
}

void test_tools_json_shape_openai_vs_anthropic() {
    PromptRenderer r;
    r.register_tool_formatter("gpt-*", std::make_shared<OpenAIToolFormatter>());
    r.register_tool_formatter("claude-*", std::make_shared<AnthropicToolFormatter>());

    ToolMeta t;
    t.name = "add";
    t.description = "add two numbers";
    t.schema = json{{"type", "object"}, {"properties", json::object()}};

    LLMInput in;
    in.system_prompt = "sys";
    in.user_prompt = "q";
    in.tools = {t};

    RenderedPrompt oai = r.render(in, "gpt-4o");
    print_input_output("tools_json_shape_openai", in, oai);
    assert(oai.tools_json.is_array());
    assert(oai.tools_json.size() == 1U);
    assert(oai.tools_json[0].at("type") == "function");
    assert(oai.tools_json[0].contains("function"));
    assert(oai.tools_json[0]["function"].at("name") == "add");
    assert(oai.tools_json[0]["function"].contains("parameters"));

    RenderedPrompt claude = r.render(in, "claude-3-7-sonnet");
    print_input_output("tools_json_shape_anthropic", in, claude);
    assert(claude.tools_json.is_array());
    assert(claude.tools_json.size() == 1U);
    assert(claude.tools_json[0].at("name") == "add");
    assert(claude.tools_json[0].contains("input_schema"));
}

void test_history_truncate_avoids_orphan_tool() {
    PromptRenderer r;
    r.set_max_history_messages(3);
    r.set_history_formatter(std::make_shared<OpenAIHistoryFormatter>());
    r.register_tool_formatter("gpt-*", std::make_shared<OpenAIToolFormatter>());

    std::vector<Message> h;
    h.push_back(Message{"user", "u1", std::nullopt, std::nullopt, 0});
    h.push_back(Message{"assistant", "{\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"x\",\"arguments\":\"{}\"}}]}",
                        std::nullopt, std::nullopt, 0});
    h.push_back(Message{"tool", "", std::optional<std::string>("x"), json{{"ok", true}}, 0});
    h.push_back(Message{"assistant", "done", std::nullopt, std::nullopt, 0});

    LLMInput in;
    in.system_prompt = "sys";
    in.user_prompt = "q";
    in.history = h;

    RenderedPrompt out = r.render(in, "gpt-4o");
    print_input_output("history_truncate_avoids_orphan_tool", in, out);
    // messages: system + truncated history + user
    // 断言：history 的第一条不应是 tool
    if (out.messages.size() >= 3U) {
        const json& first_hist = out.messages[1];
        assert(first_hist.at("role") != "tool");
    }
}

void test_image_injected_as_content_parts() {
    PromptRenderer r;
    r.register_tool_formatter("gpt-*", std::make_shared<OpenAIToolFormatter>());
    LLMInput in;
    in.system_prompt = "sys";
    in.user_prompt = "q";
    in.image_data = std::string("AAAA");
    RenderedPrompt out = r.render(in, "gpt-4o");
    print_input_output("image_injected_as_content_parts", in, out);
    const json& user = out.messages.back();
    assert(user.at("role") == "user");
    assert(user.at("content").is_array());
    assert(user.at("content").size() >= 2U);
}

} // namespace

int main() {
    test_empty_history_no_tools();
    test_context_injected_into_system();
    test_tools_json_shape_openai_vs_anthropic();
    test_history_truncate_avoids_orphan_tool();
    test_image_injected_as_content_parts();
    return 0;
}

