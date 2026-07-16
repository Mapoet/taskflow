/**
 * @file test_prompt_renderer_skill_block.cpp
 * @brief WP1.8 PromptRenderer：system 区顺序 base → Active skill → Retrieved context
 */

#include <agent/prompt_renderer/prompt_renderer.hpp>

#include <cassert>
#include <iostream>
#include <string>

int main() {
    using namespace agent_framework;

    PromptRenderer r;
    r.register_tool_formatter("gpt-*", std::make_shared<OpenAIToolFormatter>());

    LLMInput in;
    in.system_prompt = "BASE_SYS";
    in.active_skill_id = "demo";
    in.skill_block = "SKILL_BODY_LINE";
    in.user_prompt = "user_q";
    in.context = "CTX_BLOCK";
    in.tools = {};

    RenderedPrompt out = r.render(in, "gpt-4o");
    assert(!out.messages.empty());
    const std::string sys = out.messages[0].at("content").get<std::string>();

    const auto pos_base = sys.find("BASE_SYS");
    const auto pos_skill = sys.find("## Active skill (id: demo)");
    const auto pos_body = sys.find("SKILL_BODY_LINE");
    const auto pos_ctx = sys.find("## Retrieved context");
    const auto pos_ctx_data = sys.find("CTX_BLOCK");

    assert(pos_base != std::string::npos);
    assert(pos_skill != std::string::npos);
    assert(pos_body != std::string::npos);
    assert(pos_ctx != std::string::npos);
    assert(pos_ctx_data != std::string::npos);
    assert(pos_base < pos_skill);
    assert(pos_skill < pos_body);
    assert(pos_body < pos_ctx);
    assert(pos_ctx < pos_ctx_data);

    std::clog << "test_prompt_renderer_skill_block: ok\n";
    return 0;
}
