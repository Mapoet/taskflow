/**
 * @file web_fetch.cpp
 * @brief web_fetch 工具：受控 GET + 按 Content-Type 解析
 */

#include <agent/web_http.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>

namespace agent_framework {
namespace {

bool utf8_validate(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c <= 0x7FU) {
            ++i;
            continue;
        }
        const std::size_t rem = s.size() - i;
        if ((c & 0xE0U) == 0xC0U) {
            if (rem < 2 || (static_cast<unsigned char>(s[i + 1]) & 0xC0U) != 0x80U) {
                return false;
            }
            i += 2;
        } else if ((c & 0xF0U) == 0xE0U) {
            if (rem < 3 || (static_cast<unsigned char>(s[i + 1]) & 0xC0U) != 0x80U ||
                (static_cast<unsigned char>(s[i + 2]) & 0xC0U) != 0x80U) {
                return false;
            }
            i += 3;
        } else if ((c & 0xF8U) == 0xF0U) {
            if (rem < 4 || (static_cast<unsigned char>(s[i + 1]) & 0xC0U) != 0x80U ||
                (static_cast<unsigned char>(s[i + 2]) & 0xC0U) != 0x80U ||
                (static_cast<unsigned char>(s[i + 3]) & 0xC0U) != 0x80U) {
                return false;
            }
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

std::string to_hex_preview(std::string_view data, std::size_t max_bytes) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    const std::size_t n = std::min(data.size(), max_bytes);
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        const auto b = static_cast<unsigned char>(data[i]);
        out.push_back(hex[b >> 4U]);
        out.push_back(hex[b & 0xFU]);
    }
    return out;
}

std::string content_type_base(const std::string& ct) {
    std::string s = ct;
    const std::size_t semi = s.find(';');
    if (semi != std::string::npos) {
        s = s.substr(0, semi);
    }
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    return s;
}

json do_web_fetch_impl(const json& j) {
    if (!j.contains("url") || !j["url"].is_string()) {
        return web_tool_error("invalid_url", "missing url");
    }
    const std::string url = j["url"].get<std::string>();
    WebHttpConfig cfg = load_web_http_config_from_env();
    if (j.contains("max_bytes") && j["max_bytes"].is_number_integer()) {
        const int mb = j["max_bytes"].get<int>();
        if (mb > 0) {
            cfg.max_body_bytes = static_cast<std::size_t>(mb);
        }
    }

    std::map<std::string, std::string> extra;
    if (j.contains("headers") && j["headers"].is_object()) {
        for (const auto& item : j["headers"].items()) {
            if (!item.value().is_string()) {
                return web_tool_error("invalid_arguments", "headers values must be strings");
            }
            std::string lower;
            lower.reserve(item.key().size());
            for (char c : item.key()) {
                lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            if (lower == "user-agent") {
                return web_tool_error("invalid_arguments", "User-Agent not allowed in headers");
            }
            extra.emplace(item.key(), item.value().get<std::string>());
        }
    }
    if (j.contains("accept") && j["accept"].is_string()) {
        extra["Accept"] = j["accept"].get<std::string>();
    }

    const auto hres = web_http_get(url, cfg, extra);
    if (!hres.error_code.empty()) {
        json e = json{{"error", json{{"code", hres.error_code}}}};
        if (hres.error_http_status != 0) {
            e["error"]["status"] = hres.error_http_status;
        }
        return e;
    }

    const std::string ct_base = content_type_base(hres.content_type);
    if (ct_base.find("multipart/") == 0) {
        return web_tool_error("unsupported_media_type");
    }

    json out = json::object();
    out["url"] = url;
    out["final_url"] = hres.final_url;
    out["status"] = hres.status;
    out["content_type"] = hres.content_type;
    out["truncated"] = hres.truncated;

    if (ct_base == "application/json") {
        try {
            out["json"] = json::parse(hres.body);
        } catch (...) {
            return web_tool_error("invalid_json");
        }
        return out;
    }

    if (ct_base == "text/plain" || ct_base == "text/markdown" || ct_base == "text/x-markdown") {
        if (!utf8_validate(hres.body)) {
            return web_tool_error("invalid_encoding");
        }
        out["text"] = hres.body;
        return out;
    }

    if (ct_base == "text/html") {
        if (j.contains("extract_mode") && j["extract_mode"].is_string() &&
            j["extract_mode"].get<std::string>() == "main_text") {
            // 极简：去标签近似正文（契约：启发式）
            std::string t = hres.body;
            try {
                t = std::regex_replace(t, std::regex("<script[^>]*>[\\s\\S]*?</script>", std::regex::icase),
                                        "");
                t = std::regex_replace(t, std::regex("<style[^>]*>[\\s\\S]*?</style>", std::regex::icase),
                                        "");
                t = std::regex_replace(t, std::regex("<[^>]+>"), " ");
            } catch (...) {
            }
            out["text"] = t;
        } else {
            if (!utf8_validate(hres.body)) {
                out["html_base64_note"] = "non_utf8_returned_as_truncated_raw";
            }
            out["html"] = hres.body;
        }
        return out;
    }

    const std::size_t preview_cap = std::min<std::size_t>(cfg.max_body_bytes, 4096);
    out["binary_preview_hex"] = to_hex_preview(hres.body, preview_cap / 2);
    out["binary_bytes"] = hres.body.size();
    return out;
}

} // namespace

json do_web_fetch(const json& j) {
    return do_web_fetch_impl(j);
}

json web_fetch_invoke(const json& j) {
    return do_web_fetch_impl(j);
}

} // namespace agent_framework
