/**
 * @file prompt_renderer.cpp
 * @brief 提示词渲染器与格式化器实现（WP1.1 与 WP1.4 最小可用路径）
 */

#include "agent/prompt_renderer.hpp"

#include <cctype>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>

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

void trim(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
}

} // namespace

// --- StringPromptTemplate ---

StringPromptTemplate::StringPromptTemplate(const std::string& template_str)
    : template_str_(template_str), var_pattern_(R"(\{\{([^}]+)\}\})") {}

void StringPromptTemplate::load(const std::string& source) {
    template_str_ = source;
}

std::vector<std::string> StringPromptTemplate::extract_variables() const {
    std::vector<std::string> out;
    auto begin = std::sregex_iterator(template_str_.begin(), template_str_.end(), var_pattern_);
    auto end = std::sregex_iterator();
    for (auto i = begin; i != end; ++i) {
        std::string name = (*i)[1].str();
        trim(name);
        if (!name.empty()) {
            out.push_back(name);
        }
    }
    return out;
}

std::vector<std::string> StringPromptTemplate::get_variables() const {
    return extract_variables();
}

bool StringPromptTemplate::validate_variables(const std::map<std::string, std::string>& variables) const {
    for (const auto& v : extract_variables()) {
        if (variables.find(v) == variables.end()) {
            return false;
        }
    }
    return true;
}

std::string StringPromptTemplate::render(const std::map<std::string, std::string>& variables) {
    std::string out = template_str_;
    for (const auto& kv : variables) {
        const std::string ph = "{{" + kv.first + "}}";
        std::size_t pos = 0;
        while ((pos = out.find(ph, pos)) != std::string::npos) {
            out.replace(pos, ph.size(), kv.second);
            pos += kv.second.size();
        }
    }
    return out;
}

// --- FilePromptTemplate ---

FilePromptTemplate::FilePromptTemplate(const std::string& file_path) : file_path_(file_path) {
    inner_template_ = std::make_shared<StringPromptTemplate>("");
    load(file_path);
}

void FilePromptTemplate::load(const std::string& source) {
    file_path_ = source;
    std::ifstream in(file_path_);
    if (!in) {
        throw std::runtime_error("FilePromptTemplate: cannot read " + file_path_);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    inner_template_ = std::make_shared<StringPromptTemplate>(ss.str());
}

std::string FilePromptTemplate::render(const std::map<std::string, std::string>& variables) {
    return inner_template_->render(variables);
}

std::vector<std::string> FilePromptTemplate::get_variables() const {
    return inner_template_->get_variables();
}

bool FilePromptTemplate::validate_variables(const std::map<std::string, std::string>& variables) const {
    return inner_template_->validate_variables(variables);
}

// --- OpenAIToolFormatter ---

json OpenAIToolFormatter::convert_to_openai_format(const ToolMeta& tool) {
    json params = tool.schema.is_null() || tool.schema.empty() ? json::object() : tool.schema;
    json fn = json{{"name", tool.name}, {"description", tool.description}, {"parameters", std::move(params)}};
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

// --- AnthropicToolFormatter ---

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

// --- GeminiToolFormatter ---

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

// --- OpenAIHistoryFormatter ---

std::string OpenAIHistoryFormatter::format_as_text(const std::vector<Message>& history) {
    std::ostringstream o;
    for (const auto& m : history) {
        o << m.role << ": " << m.content << '\n';
    }
    return o.str();
}

std::vector<json> OpenAIHistoryFormatter::format_as_messages(const std::vector<Message>& history) {
    std::vector<json> out;
    out.reserve(history.size());
    for (const auto& m : history) {
        json j = json{{"role", m.role}, {"content", m.content}};
        if (m.role == "tool") {
            j["name"] = m.tool_name.value_or("");
            if (m.tool_result) {
                j["content"] = m.tool_result->dump();
            }
        }
        out.push_back(std::move(j));
    }
    return out;
}

std::vector<Message> OpenAIHistoryFormatter::truncate(const std::vector<Message>& history,
                                                      int max_messages) {
    if (max_messages <= 0 || static_cast<int>(history.size()) <= max_messages) {
        return history;
    }
    return std::vector<Message>(history.end() - max_messages, history.end());
}

// --- PromptRenderer ---

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
    {
        std::lock_guard<std::mutex> lock(formatters_mutex_);
        tpl = template_;
        hist_fmt = history_formatter_;
    }

    std::map<std::string, std::string> vars;
    vars["system_prompt"] = input.system_prompt;
    vars["user_prompt"] = input.user_prompt;
    vars["context"] = input.context;
    vars["tools_text"] = OpenAIToolFormatter().format_tools_as_text(input.tools);

    RenderedPrompt rendered;
    rendered.rendered_text = tpl->render(vars);

    std::shared_ptr<HistoryFormatter> hf =
        hist_fmt ? hist_fmt : std::make_shared<OpenAIHistoryFormatter>();

    rendered.messages.clear();
    if (!input.system_prompt.empty()) {
        rendered.messages.push_back(
            json{{"role", "system"}, {"content", input.system_prompt}});
    }
    for (const auto& jm : hf->format_as_messages(input.history)) {
        rendered.messages.push_back(jm);
    }
    std::string user_block = input.user_prompt;
    if (!input.context.empty()) {
        user_block = input.context + "\n\n" + user_block;
    }
    rendered.messages.push_back(json{{"role", "user"}, {"content", user_block}});

    rendered.tools_json = OpenAIToolFormatter().format_tools(input.tools);
    rendered.image_data = input.image_data;
    rendered.audio_data = input.audio_data;
    integrate_multimodal_input(rendered, input);
    rendered.total_tokens = estimate_tokens(rendered);
    return truncate_prompt(rendered, model_name);
}

} // namespace agent_framework
