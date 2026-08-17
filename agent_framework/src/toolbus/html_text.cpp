#include <agent/internal/html_text.hpp>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace agent_framework::internal {
namespace {

char ascii_lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool ascii_starts_with(std::string_view input, std::size_t pos, std::string_view needle) {
    if (pos > input.size() || needle.size() > input.size() - pos) return false;
    for (std::size_t i = 0; i < needle.size(); ++i) {
        if (ascii_lower(input[pos + i]) != ascii_lower(needle[i])) return false;
    }
    return true;
}

std::size_t ascii_find(std::string_view input, std::string_view needle, std::size_t pos,
                       std::size_t& operations, std::size_t max_operations) {
    if (needle.empty()) return pos;
    while (pos + needle.size() <= input.size()) {
        if (++operations > max_operations) return std::string_view::npos;
        if (ascii_starts_with(input, pos, needle)) return pos;
        ++pos;
    }
    return std::string_view::npos;
}

void append_space(std::string& out, std::size_t cap, bool& truncated) {
    if (out.empty() || out.back() == ' ') return;
    if (out.size() >= cap) { truncated = true; return; }
    out.push_back(' ');
}

void append_char(std::string& out, char c, std::size_t cap, bool& truncated) {
    if (std::isspace(static_cast<unsigned char>(c))) {
        append_space(out, cap, truncated);
    } else if (out.size() < cap) {
        out.push_back(c);
    } else {
        truncated = true;
    }
}

bool append_entity(std::string_view html, std::size_t& pos, std::string& out,
                   std::size_t cap, bool& truncated) {
    static constexpr std::pair<std::string_view, char> entities[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'},
        {"&#39;", '\''}, {"&apos;", '\''}, {"&nbsp;", ' '}};
    for (const auto& entity : entities) {
        if (html.substr(pos, entity.first.size()) == entity.first) {
            append_char(out, entity.second, cap, truncated);
            pos += entity.first.size();
            return true;
        }
    }
    return false;
}

std::string_view tag_name(std::string_view tag) {
    std::size_t p = 1;
    if (p < tag.size() && tag[p] == '/') ++p;
    while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p]))) ++p;
    const std::size_t begin = p;
    while (p < tag.size() && (std::isalnum(static_cast<unsigned char>(tag[p])) || tag[p] == '-')) ++p;
    return tag.substr(begin, p - begin);
}

bool name_is(std::string_view name, std::string_view expected) {
    return name.size() == expected.size() && ascii_starts_with(name, 0, expected);
}

} // namespace

HtmlTextResult extract_html_text(std::string_view html, const HtmlTextLimits& limits) {
    HtmlTextResult result;
    if (limits.max_input_bytes == 0 || limits.max_output_bytes == 0 ||
        limits.max_tag_bytes == 0 || limits.max_operations == 0) {
        result.error_code = "html_parse_budget_invalid";
        return result;
    }
    if (html.size() > limits.max_input_bytes) {
        html = html.substr(0, limits.max_input_bytes);
        result.truncated = true;
    }
    result.text.reserve(std::min(html.size(), limits.max_output_bytes));
    std::size_t operations = 0;
    std::size_t pos = 0;
    while (pos < html.size()) {
        if (++operations > limits.max_operations) {
            result.error_code = "html_parse_budget_exceeded";
            result.truncated = true;
            break;
        }
        if (html[pos] == '&' && append_entity(html, pos, result.text,
                                               limits.max_output_bytes, result.truncated)) continue;
        if (html[pos] != '<') {
            append_char(result.text, html[pos++], limits.max_output_bytes, result.truncated);
            continue;
        }
        if (html.substr(pos, 4) == "<!--") {
            const auto end = html.find("-->", pos + 4);
            pos = end == std::string_view::npos ? html.size() : end + 3;
            continue;
        }
        const auto close = html.find('>', pos + 1);
        if (close == std::string_view::npos) {
            result.truncated = true;
            break;
        }
        if (close - pos + 1 > limits.max_tag_bytes) {
            result.error_code = "html_tag_too_large";
            result.truncated = true;
            break;
        }
        const auto tag = html.substr(pos, close - pos + 1);
        const auto name = tag_name(tag);
        const bool closing = tag.size() > 1 && tag[1] == '/';
        pos = close + 1;
        if (!closing && (name_is(name, "script") || name_is(name, "style"))) {
            const std::string_view end_tag = name_is(name, "script") ? "</script" : "</style";
            const auto end = ascii_find(html, end_tag, pos, operations, limits.max_operations);
            if (end == std::string_view::npos) {
                if (operations > limits.max_operations) result.error_code = "html_parse_budget_exceeded";
                result.truncated = true;
                break;
            }
            const auto end_close = html.find('>', end + end_tag.size());
            pos = end_close == std::string_view::npos ? html.size() : end_close + 1;
        }
        append_space(result.text, limits.max_output_bytes, result.truncated);
    }
    while (!result.text.empty() && result.text.back() == ' ') result.text.pop_back();
    result.scanned_bytes = pos;
    return result;
}

std::string strip_html_tags_bounded(std::string_view html, std::size_t max_output_bytes) {
    HtmlTextLimits limits;
    limits.max_input_bytes = html.size();
    limits.max_output_bytes = max_output_bytes;
    limits.max_tag_bytes = std::min<std::size_t>(16384, std::max<std::size_t>(1, html.size()));
    limits.max_operations = std::max<std::size_t>(1024, html.size() * 4);
    return extract_html_text(html, limits).text;
}

} // namespace agent_framework::internal
