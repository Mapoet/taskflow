/**
 * @file skill_frontmatter_parse.cpp
 * @brief WP1.8 受限 frontmatter 解析（Cursor：name、description:>-、disable-model-invocation）
 */

#include <agent/internal/skill_frontmatter_parse.hpp>

#include <cctype>
#include <sstream>
#include <vector>

namespace agent_framework {
namespace internal {

namespace {

void trim_inplace(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
}

std::string trim_copy(std::string_view sv) {
    std::size_t a = 0;
    while (a < sv.size() && std::isspace(static_cast<unsigned char>(sv[a]))) {
        ++a;
    }
    std::size_t b = sv.size();
    while (b > a && std::isspace(static_cast<unsigned char>(sv[b - 1]))) {
        --b;
    }
    return std::string(sv.substr(a, b - a));
}

std::string lower_copy(std::string_view sv) {
    std::string o;
    o.reserve(sv.size());
    for (unsigned char c : sv) {
        o.push_back(static_cast<char>(std::tolower(c)));
    }
    return o;
}

std::string normalize_yaml_key(std::string_view key) {
    std::string o;
    for (char c : key) {
        o += (c == '-') ? '_' : c;
    }
    return o;
}

bool parse_bool_scalar(std::string_view rest) {
    const std::string t = lower_copy(trim_copy(rest));
    return t == "true" || t == "yes" || t == "1";
}

bool looks_like_new_top_level_key(std::string_view trimmed) {
    if (trimmed.empty() || trimmed[0] == '-') {
        return false;
    }
    const std::size_t c = trimmed.find(':');
    if (c == std::string::npos) {
        return false;
    }
    std::string key = trim_copy(trimmed.substr(0, c));
    if (key.empty()) {
        return false;
    }
    return true;
}

/** Strip leading spaces/tabs (YAML folded / literal continuation indent). */
std::string strip_indent(std::string_view line) {
    std::size_t k = 0;
    while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) {
        ++k;
    }
    return std::string(line.substr(k));
}

/** Collect folded (>) / literal (|) lines after `key: >-` line; returns index of first line *after* block. */
std::size_t consume_block_scalar(const std::vector<std::string>& lines,
                                  std::size_t j,
                                  bool literal_fold,
                                  std::string* out_accum) {
    std::vector<std::string> parts;
    while (j < lines.size()) {
        const std::string& L = lines[j];
        if (!L.empty() && L[0] != ' ' && L[0] != '\t') {
            const std::string t = trim_copy(L);
            if (looks_like_new_top_level_key(t)) {
                break;
            }
        }
        if (L.empty()) {
            if (literal_fold) {
                parts.emplace_back("");
                ++j;
                continue;
            }
            break;
        }
        if (L[0] == ' ' || L[0] == '\t') {
            parts.push_back(strip_indent(L));
            ++j;
            continue;
        }
        break;
    }
    if (literal_fold) {
        // `|` literal: preserve newlines
        std::string out;
        for (std::size_t p = 0; p < parts.size(); ++p) {
            if (p > 0) {
                out += '\n';
            }
            out += parts[p];
        }
        *out_accum = std::move(out);
    } else {
        std::string out;
        for (const auto& part : parts) {
            if (!out.empty()) {
                out += ' ';
            }
            out += trim_copy(part);
        }
        trim_inplace(out);
        *out_accum = std::move(out);
    }
    return j;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string ln;
    while (std::getline(iss, ln)) {
        if (!ln.empty() && ln.back() == '\r') {
            ln.pop_back();
        }
        out.push_back(std::move(ln));
    }
    return out;
}

bool is_list_item(std::string_view line, std::string& out_item) {
    std::string t = trim_copy(line);
    if (t.size() < 2 || t[0] != '-') {
        return false;
    }
    if (t.size() >= 2 && t[1] != ' ') {
        return false;
    }
    out_item = trim_copy(std::string_view(t).substr(2));
    return true;
}

bool is_block_indicator(std::string_view rest) {
    const std::string t = trim_copy(rest);
    return t == ">-" || t == ">" || t == "|";
}

} // namespace

SplitFrontmatterResult split_skill_file_content(std::string_view file_content) {
    SplitFrontmatterResult r;
    const std::string content(file_content);
    if (content.size() < 7) {
        return r;
    }
    std::size_t yaml_start = 0;
    if (content.compare(0, 4, "---\n") == 0) {
        yaml_start = 4;
    } else if (content.size() >= 5 && content.compare(0, 5, "---\r\n") == 0) {
        yaml_start = 5;
    } else if (content.size() >= 3 && content.compare(0, 3, "---") == 0) {
        yaml_start = 3;
        if (yaml_start < content.size() && content[yaml_start] == '\r') {
            ++yaml_start;
        }
        if (yaml_start < content.size() && content[yaml_start] == '\n') {
            ++yaml_start;
        }
    } else {
        return r;
    }

    const std::size_t nl3a = content.find("\n---\n", yaml_start);
    const std::size_t nl3b = content.find("\n---\r\n", yaml_start);
    std::size_t close_nl = std::string::npos;
    std::size_t body_off = 0;
    if (nl3a != std::string::npos && (nl3b == std::string::npos || nl3a <= nl3b)) {
        close_nl = nl3a;
        body_off = nl3a + 5;
    } else if (nl3b != std::string::npos) {
        close_nl = nl3b;
        body_off = nl3b + 6;
    }

    if (close_nl == std::string::npos) {
        r.yaml_inner = content.substr(yaml_start);
        trim_inplace(r.yaml_inner);
        r.body.clear();
        r.ok = true;
        return r;
    }

    r.yaml_inner = content.substr(yaml_start, close_nl - yaml_start);
    trim_inplace(r.yaml_inner);
    r.body = content.substr(body_off);
    r.ok = true;
    return r;
}

std::optional<SkillIndexEntry> parse_skill_frontmatter_yaml(const std::string& yaml_block,
                                                            std::string* error_out) {
    (void)error_out;
    const std::string trimmed_block = trim_copy(yaml_block);
    if (trimmed_block.empty()) {
        return std::nullopt;
    }

    SkillIndexEntry e;
    const std::vector<std::string> lines = split_lines(yaml_block);

    enum class Mode { None, Keywords, Tags };
    Mode mode = Mode::None;

    std::size_t i = 0;
    while (i < lines.size()) {
        std::string line = lines[i];
        std::string trimmed = trim_copy(line);

        if (trimmed.empty()) {
            ++i;
            continue;
        }

        if (mode != Mode::None) {
            std::string item;
            if (is_list_item(trimmed, item)) {
                if (mode == Mode::Keywords) {
                    e.trigger_keywords.push_back(std::move(item));
                } else {
                    e.tags.push_back(std::move(item));
                }
                ++i;
                continue;
            }
            mode = Mode::None;
        }

        const std::size_t col = trimmed.find(':');
        if (col == std::string::npos) {
            ++i;
            continue;
        }
        std::string key = trim_copy(std::string_view(trimmed).substr(0, col));
        std::string rest = trim_copy(std::string_view(trimmed).substr(col + 1));
        const std::string key_norm = normalize_yaml_key(key);

        if (key_norm == "disable_model_invocation") {
            e.disable_model_invocation = parse_bool_scalar(rest);
            ++i;
            continue;
        }

        if (key == "id") {
            e.yaml_id = rest;
            ++i;
            continue;
        }

        if (key == "name") {
            if (is_block_indicator(rest)) {
                const bool lit = (trim_copy(rest) == "|");
                std::string block_text;
                const std::size_t j = consume_block_scalar(lines, i + 1, lit, &block_text);
                e.name = std::move(block_text);
                i = j;
            } else {
                e.name = rest;
                ++i;
            }
            continue;
        }

        if (key == "description") {
            if (is_block_indicator(rest)) {
                const bool lit = (trim_copy(rest) == "|");
                std::string block_text;
                const std::size_t j = consume_block_scalar(lines, i + 1, lit, &block_text);
                e.description = std::move(block_text);
                i = j;
            } else {
                e.description = rest;
                ++i;
            }
            continue;
        }

        if (key == "trigger_keywords") {
            if (rest.empty()) {
                mode = Mode::Keywords;
            }
            ++i;
            continue;
        }
        if (key == "tags") {
            if (rest.empty()) {
                mode = Mode::Tags;
            }
            ++i;
            continue;
        }
        if (key == "resources") {
            if (!rest.empty()) {
                try {
                    e.resources = nlohmann::json::parse(rest);
                } catch (...) {
                    e.resources = nlohmann::json{{"raw", rest}};
                }
            }
            ++i;
            continue;
        }

        ++i;
    }

    return e;
}

} // namespace internal
} // namespace agent_framework
