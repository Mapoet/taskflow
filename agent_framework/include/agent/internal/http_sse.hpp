/**
 * @file http_sse.hpp
 * @brief HTTP SSE（text/event-stream）body 最小解析工具（internal）
 * @author Mapoet
 * @version 0.1
 * @date 2026-03-31
 */
#ifndef __AGENT_INTERNAL_HTTP_SSE_HPP__
#define __AGENT_INTERNAL_HTTP_SSE_HPP__

#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace agent_framework {
namespace internal {

inline bool icontains(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    if (haystack.size() < needle.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            char a = haystack[i + j];
            char b = needle[j];
            if (std::tolower(static_cast<unsigned char>(a)) !=
                std::tolower(static_cast<unsigned char>(b))) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

inline std::string ltrim_one_space(std::string s) {
    if (!s.empty() && s.front() == ' ') {
        s.erase(s.begin());
    }
    return s;
}

/**
 * @brief Parse SSE response body to a single JSON text by concatenating all `data:` lines with '\n'.
 *
 * Rules:
 * - Split by lines, support '\n' and '\r\n'
 * - Collect `data:` lines; remove one optional leading space after `data:`
 * - Stop when the value equals "[DONE]"
 * - Ignore other fields (event/id/retry) and comment lines starting with ':'
 * - If no `data:` lines exist, throw.
 */
inline std::string parse_sse_body_to_json_text(std::string_view body) {
    std::vector<std::string> data_lines;
    data_lines.reserve(16);

    std::size_t i = 0;
    while (i < body.size()) {
        std::size_t line_end = body.find('\n', i);
        if (line_end == std::string_view::npos) {
            line_end = body.size();
        }
        std::size_t line_len = line_end - i;
        if (line_len > 0 && body[i + line_len - 1] == '\r') {
            --line_len;
        }
        std::string_view line = body.substr(i, line_len);
        i = (line_end == body.size()) ? body.size() : (line_end + 1);

        if (line.empty()) {
            continue;
        }
        if (line.front() == ':') {
            continue;
        }
        constexpr std::string_view k_data = "data:";
        if (line.size() >= k_data.size() && icontains(line.substr(0, k_data.size()), k_data)) {
            std::string v = ltrim_one_space(std::string(line.substr(k_data.size())));
            if (v == "[DONE]") {
                break;
            }
            data_lines.push_back(std::move(v));
            continue;
        }
        // event:/id:/retry: ignored
    }

    if (data_lines.empty()) {
        throw std::runtime_error("HttpMCPTransport: SSE body missing data lines");
    }

    std::ostringstream joined;
    for (std::size_t k = 0; k < data_lines.size(); ++k) {
        if (k > 0) {
            joined << '\n';
        }
        joined << data_lines[k];
    }
    return joined.str();
}

} // namespace internal
} // namespace agent_framework

#endif // __AGENT_INTERNAL_HTTP_SSE_HPP__
