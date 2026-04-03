/**
 * @file skill_frontmatter_parse.cpp
 * @brief WP1.8 受限 frontmatter 解析
 */

#include <agent/internal/skill_frontmatter_parse.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>

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
    if (nl3a != std::string::npos &&
        (nl3b == std::string::npos || nl3a <= nl3b)) {
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
    SkillIndexEntry e;
    std::istringstream in(yaml_block);
    std::string line;
    enum class Mode { None, Keywords, Tags };
    Mode mode = Mode::None;

    auto fail = [&](const char* msg) -> std::optional<SkillIndexEntry> {
        if (error_out) {
            *error_out = msg;
        }
        return std::nullopt;
    };

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::string trimmed = trim_copy(line);
        if (trimmed.empty()) {
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
                continue;
            }
            mode = Mode::None;
        }

        const std::size_t col = trimmed.find(':');
        if (col == std::string::npos) {
            continue;
        }
        std::string key = trim_copy(std::string_view(trimmed).substr(0, col));
        std::string rest = trim_copy(std::string_view(trimmed).substr(col + 1));

        if (key == "id") {
            if (rest.empty()) {
                return fail("id empty");
            }
            e.id = std::move(rest);
        } else if (key == "name") {
            e.name = std::move(rest);
        } else if (key == "description") {
            e.description = std::move(rest);
        } else if (key == "trigger_keywords") {
            if (rest.empty()) {
                mode = Mode::Keywords;
            }
        } else if (key == "tags") {
            if (rest.empty()) {
                mode = Mode::Tags;
            }
        } else if (key == "resources") {
            if (!rest.empty()) {
                try {
                    e.resources = nlohmann::json::parse(rest);
                } catch (...) {
                    e.resources = nlohmann::json{{"raw", rest}};
                }
            }
        }
    }

    if (e.id.empty()) {
        return fail("missing id");
    }
    return e;
}

} // namespace internal
} // namespace agent_framework
