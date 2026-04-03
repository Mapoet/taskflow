/**
 * @file prompt_renderer.cpp
 * @brief 提示词渲染器与格式化器实现（WP1.1 与 WP1.4 最小可用路径）
 */

#include "agent/prompt_renderer.hpp"

#include <cctype>
#include <iostream>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace agent_framework {

namespace {

bool wildcard_match(const std::string& pattern, const std::string& model) {
    if (pattern == "*" || pattern.empty()) {
        return true;
    }
    if (pattern.size() >= 2 && pattern.back() == '*') {
        const std::string pref = pattern.substr(0, pattern.size() - 1);
        return model.size() >= pref.size() && model.compare(0, pref.size(), pref) == 0;
    }
    if (pattern.size() >= 2 && pattern.front() == '*') {
        const std::string suf = pattern.substr(1);
        return model.size() >= suf.size() &&
               model.compare(model.size() - suf.size(), suf.size(), suf) == 0;
    }
    return pattern == model;
}

std::vector<std::string> scan_template_vars(std::string_view text) {
    static const std::regex k_var_pattern(R"(\{\{([^}]+)\}\})");
    std::vector<std::string> out;
    std::string s(text);
    auto begin = std::sregex_iterator(s.begin(), s.end(), k_var_pattern);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string name = (*it)[1].str();
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) {
            name.erase(name.begin());
        }
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) {
            name.pop_back();
        }
        if (!name.empty()) {
            out.push_back(std::move(name));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string render_user_template(std::string text,
                                 const std::map<std::string, std::string>& user_vars,
                                 std::vector<std::string>& missing_vars_out) {
    missing_vars_out.clear();
    const std::vector<std::string> vars = scan_template_vars(text);
    missing_vars_out.reserve(vars.size());
    for (const auto& k : vars) {
        auto it = user_vars.find(k);
        if (it == user_vars.end()) {
            missing_vars_out.push_back(k);
            continue;
        }
        const std::string ph = "{{" + k + "}}";
        std::size_t pos = 0;
        while ((pos = text.find(ph, pos)) != std::string::npos) {
            text.replace(pos, ph.size(), it->second);
            pos += it->second.size();
        }
    }
    return text;
}

std::string build_missing_vars_system_notice(const std::vector<std::string>& missing_vars) {
    std::ostringstream o;
    o << "Template variables are missing and were not fully rendered.\n";
    o << "Missing:\n";
    for (const auto& v : missing_vars) {
        o << "- {{" << v << "}}\n";
    }
    o << "\nInstruction:\n";
    o << "You MUST decide how to proceed. If these variables are required to answer accurately,\n";
    o << "ask the user concise clarification questions to obtain them. Otherwise, proceed with\n";
    o << "reasonable assumptions and clearly state what you assumed.\n";
    return o.str();
}

} // namespace

// --- PromptRenderer ---

PromptRenderer::PromptRenderer()
    : template_(std::make_shared<StringPromptTemplate>(
          "{{system_prompt}}\n\n{{tools_text}}\n\n{{context}}\n\n{{user_prompt}}")) {
    // Default formatters to avoid noisy fallback logs for common models.
    // Matching is prefix-wildcard (e.g. "gpt-*").
    register_tool_formatter("gpt-*", std::make_shared<OpenAIToolFormatter>());
    register_tool_formatter("openai-*", std::make_shared<OpenAIToolFormatter>());
    register_tool_formatter("deepseek-*", std::make_shared<OpenAIToolFormatter>());
    register_tool_formatter("claude-*", std::make_shared<AnthropicToolFormatter>());
}

PromptRenderer::PromptRenderer(std::shared_ptr<PromptTemplate> template_ptr)
    : template_(std::move(template_ptr)) {
    if (!template_) {
        throw std::invalid_argument("PromptRenderer: template_ptr is null");
    }
}

void PromptRenderer::register_tool_formatter(const std::string& model_pattern,
                                               std::shared_ptr<ToolFormatter> formatter) {
    std::lock_guard<std::mutex> lock(formatters_mutex_);
    tool_formatters_[model_pattern] = std::move(formatter);
}

void PromptRenderer::set_history_formatter(std::shared_ptr<HistoryFormatter> formatter) {
    std::lock_guard<std::mutex> lock(formatters_mutex_);
    history_formatter_ = std::move(formatter);
}

void PromptRenderer::set_template(std::shared_ptr<PromptTemplate> template_ptr) {
    if (!template_ptr) {
        throw std::invalid_argument("PromptRenderer::set_template: null");
    }
    std::lock_guard<std::mutex> lock(formatters_mutex_);
    template_ = std::move(template_ptr);
}

void PromptRenderer::set_max_tokens(const std::string& model_name, int max_tokens) {
    std::lock_guard<std::mutex> lock(formatters_mutex_);
    max_tokens_map_[model_name] = max_tokens;
}

void PromptRenderer::set_max_history_messages(int max_history_messages) {
    std::lock_guard<std::mutex> lock(formatters_mutex_);
    max_history_messages_ = max_history_messages;
}

std::shared_ptr<ToolFormatter> PromptRenderer::get_tool_formatter(const std::string& model_name) {
    for (const auto& kv : tool_formatters_) {
        if (wildcard_match(kv.first, model_name)) {
            return kv.second;
        }
    }
    return nullptr;
}

int PromptRenderer::estimate_tokens(const RenderedPrompt& rendered) {
    int n = static_cast<int>(rendered.rendered_text.size() / 4);
    for (const auto& m : rendered.messages) {
        n += static_cast<int>(m.dump().size() / 4);
    }
    return n;
}

RenderedPrompt PromptRenderer::truncate_prompt(const RenderedPrompt& rendered,
                                               const std::string& model_name) {
    int budget = -1;
    {
        std::lock_guard<std::mutex> lock(formatters_mutex_);
        auto it = max_tokens_map_.find(model_name);
        if (it != max_tokens_map_.end()) {
            budget = it->second;
        }
    }
    (void)budget;
    return rendered;
}

void PromptRenderer::integrate_multimodal_input(RenderedPrompt& rendered, const LLMInput& input) {
    if (!input.image_data && !input.audio_data) {
        return;
    }
    if (rendered.messages.empty()) {
        return;
    }
    json& last = rendered.messages.back();
    if (last.value("role", "") != "user") {
        return;
    }
    json parts = json::array();
    if (last.contains("content") && last["content"].is_string()) {
        parts.push_back(json{{"type", "text"}, {"text", last["content"].get<std::string>()}});
    }
    if (input.image_data) {
        parts.push_back(
            json{{"type", "image_url"},
                 {"image_url", json{{"url", "data:image/jpeg;base64," + *input.image_data}}}});
    }
    if (input.audio_data) {
        parts.push_back(json{
            {"type", "input_audio"},
            {"input_audio", json{{"data", *input.audio_data}, {"format", "wav"}}}});
    }
    last["content"] = std::move(parts);
}

RenderedPrompt PromptRenderer::render(const LLMInput& input, const std::string& model_name) {
    std::shared_ptr<PromptTemplate> tpl;
    std::shared_ptr<HistoryFormatter> hist_fmt;
    int max_hist = 20;
    {
        std::lock_guard<std::mutex> lock(formatters_mutex_);
        tpl = template_;
        hist_fmt = history_formatter_;
        max_hist = max_history_messages_;
    }

    std::map<std::string, std::string> vars;
    std::vector<std::string> missing_sys;
    std::vector<std::string> missing_user;
    std::string user_system_prompt =
        render_user_template(input.system_prompt, input.extra_variables, missing_sys);
    std::string user_user_prompt =
        render_user_template(input.user_prompt, input.extra_variables, missing_user);

    std::vector<std::string> missing_all = missing_sys;
    missing_all.insert(missing_all.end(), missing_user.begin(), missing_user.end());
    std::sort(missing_all.begin(), missing_all.end());
    missing_all.erase(std::unique(missing_all.begin(), missing_all.end()), missing_all.end());

    std::string system_block = user_system_prompt;
    if (input.skill_block && !input.skill_block->empty()) {
        const std::string sid =
            (input.active_skill_id && !input.active_skill_id->empty()) ? *input.active_skill_id
                                                                       : std::string("unknown");
        system_block += "\n\n## Active skill (id: ";
        system_block += sid;
        system_block += ")\n";
        system_block += *input.skill_block;
    }
    if (!input.context.empty()) {
        system_block += "\n\n## Retrieved context\n";
        system_block += input.context;
    }
    vars["system_prompt"] = system_block;
    vars["user_prompt"] = user_user_prompt;
    vars["context"] = input.context;
    vars["tools_text"] = OpenAIToolFormatter().format_tools_as_text(input.tools);

    RenderedPrompt rendered;
    rendered.rendered_text = tpl->render(vars);

    std::shared_ptr<HistoryFormatter> hf =
        hist_fmt ? hist_fmt : std::make_shared<OpenAIHistoryFormatter>();

    rendered.messages.clear();
    if (!system_block.empty()) {
        rendered.messages.push_back(json{{"role", "system"}, {"content", system_block}});
    }
    if (!missing_all.empty()) {
        rendered.messages.push_back(
            json{{"role", "system"}, {"content", build_missing_vars_system_notice(missing_all)}});
    }
    const std::vector<Message> history_trunc = hf->truncate(input.history, max_hist);
    for (const auto& jm : hf->format_as_messages(history_trunc)) {
        rendered.messages.push_back(jm);
    }
    rendered.messages.push_back(json{{"role", "user"}, {"content", user_user_prompt}});

    std::shared_ptr<ToolFormatter> tf = get_tool_formatter(model_name);
    if (!tf) {
        std::clog << "PromptRenderer: no ToolFormatter for model \"" << model_name
                  << "\", fallback to OpenAIToolFormatter\n";
        tf = std::make_shared<OpenAIToolFormatter>();
    }
    rendered.tools_json = tf->format_tools(input.tools);
    rendered.image_data = input.image_data;
    rendered.audio_data = input.audio_data;
    integrate_multimodal_input(rendered, input);
    rendered.total_tokens = estimate_tokens(rendered);
    return truncate_prompt(rendered, model_name);
}

} // namespace agent_framework
