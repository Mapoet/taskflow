/**
 * @file prompt_template.cpp
 * @brief 提示词模板实现
 */
#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <agent/prompt_renderer/prompt_renderer.hpp>

namespace agent_framework {

namespace {

void trim(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
}

std::vector<std::string> scan_template_vars(std::string_view text) {
    static const std::regex k_var_pattern(R"(\{\{([^}]+)\}\})");
    std::vector<std::string> out;
    std::string s(text);
    auto begin = std::sregex_iterator(s.begin(), s.end(), k_var_pattern);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string name = (*it)[1].str();
        trim(name);
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
        // also support "{{ k }}" with spaces inside braces by brute normalization is not done in v1
    }
    return text;
}

} // namespace

StringPromptTemplate::StringPromptTemplate(const std::string& template_str)
    : template_str_(template_str), var_pattern_(R"(\{\{([^}]+)\}\})") {}

void StringPromptTemplate::load(const std::string& source) {
    template_str_ = source;
}

std::vector<std::string> StringPromptTemplate::extract_variables() const {
    std::vector<std::string> out;
    auto begin = std::sregex_iterator(template_str_.begin(), template_str_.end(), var_pattern_);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string name = (*it)[1].str();
        trim(name);
        if (!name.empty()) {
            out.push_back(std::move(name));
        }
    }
    return out;
}

std::vector<std::string> StringPromptTemplate::get_variables() const {
    return extract_variables();
}

bool StringPromptTemplate::validate_variables(
    const std::map<std::string, std::string>& variables) const {
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

bool FilePromptTemplate::validate_variables(
    const std::map<std::string, std::string>& variables) const {
    return inner_template_->validate_variables(variables);
}

} // namespace agent_framework

