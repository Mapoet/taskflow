/**
 * @file web_search_ddg.cpp
 * @brief DuckDuckGo html.duckduckgo.com 搜索（不重定向到非 duckduckgo.com）
 */

#include <agent/web_http.hpp>
#include <agent/web_search_ddg.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <regex>
#include <thread>

#if __has_include(<httplib/httplib.hpp>)
#include <httplib/httplib.hpp>
#elif __has_include(<httplib.hpp>)
#include <httplib.hpp>
#elif __has_include(<httplib.h>)
#include <httplib.h>
#endif

namespace agent_framework {
namespace {

std::mutex g_ddg_rate_mu;
std::chrono::steady_clock::time_point g_ddg_last{};
bool g_ddg_last_init = false;

int env_int_local(const char* key, int def_v) {
    const char* v = std::getenv(key);
    if (!v || !*v) {
        return def_v;
    }
    return std::atoi(v);
}

std::string env_str_local(const char* key, const char* def_v) {
    const char* v = std::getenv(key);
    if (v && *v) {
        return std::string(v);
    }
    return std::string(def_v);
}

void trim_inplace_str(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
}

int ddg_min_interval_ms() {
    const char* v = std::getenv("AGENT_WEB_SEARCH_MIN_INTERVAL_MS");
    if (!v || !*v) {
        return 1000;
    }
    const int n = std::atoi(v);
    return n < 0 ? 1000 : n;
}

int ddg_search_timeout_ms() {
    const char* v = std::getenv("AGENT_WEB_SEARCH_TIMEOUT_MS");
    if (v && *v) {
        return std::atoi(v);
    }
    return env_int_local("AGENT_WEB_TIMEOUT_MS", 30000);
}

std::size_t ddg_max_body_bytes() {
    const char* v = std::getenv("AGENT_WEB_DDG_MAX_BODY_BYTES");
    if (v && *v) {
        const long n = std::strtol(v, nullptr, 10);
        if (n > 0) {
            return static_cast<std::size_t>(n);
        }
    }
    return 1048576;
}

std::string url_encode_query(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else if (c == ' ') {
            out += '+';
        } else {
            out += '%';
            out += hex[c >> 4U];
            out += hex[c & 15U];
        }
    }
    return out;
}

std::string html_decode_basic(std::string s) {
    static const std::pair<const char*, const char*> entities[] = {
        {"&amp;", "&"},   {"&lt;", "<"},      {"&gt;", ">"},
        {"&quot;", "\""}, {"&#39;", "'"},     {"&apos;", "'"},
        {"&#x27;", "'"}, {"&#x2F;", "/"},
    };
    for (const auto& e : entities) {
        std::size_t pos = 0;
        const std::string from = e.first;
        const std::string to = e.second;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    return s;
}

std::string strip_html_tags(const std::string& input) {
    try {
        return std::regex_replace(input, std::regex("<[^>]*>"), "");
    } catch (...) {
        return input;
    }
}

std::string url_decode_component(const std::string& enc) {
    std::string out;
    out.reserve(enc.size());
    for (std::size_t i = 0; i < enc.size(); ++i) {
        if (enc[i] == '%' && i + 2 < enc.size()) {
            int hi = std::tolower(static_cast<unsigned char>(enc[i + 1]));
            int lo = std::tolower(static_cast<unsigned char>(enc[i + 2]));
            auto hexv = [](int c) -> int {
                if (c >= '0' && c <= '9') {
                    return c - '0';
                }
                if (c >= 'a' && c <= 'f') {
                    return 10 + (c - 'a');
                }
                return -1;
            };
            const int vh = hexv(hi);
            const int vl = hexv(lo);
            if (vh >= 0 && vl >= 0) {
                out += static_cast<char>((vh << 4) | vl);
                i += 2;
                continue;
            }
        } else if (enc[i] == '+') {
            out += ' ';
        } else {
            out += enc[i];
        }
    }
    return out;
}

std::string unwrap_ddg_redirect_url(std::string url) {
    const std::string needle = "uddg=";
    const std::size_t p = url.find(needle);
    if (p == std::string::npos) {
        return url;
    }
    std::string enc = url.substr(p + needle.size());
    const std::size_t amp = enc.find('&');
    if (amp != std::string::npos) {
        enc = enc.substr(0, amp);
    }
    return url_decode_component(enc);
}

void ddg_wait_rate_limit() {
    const int min_ms = ddg_min_interval_ms();
    if (min_ms <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_ddg_rate_mu);
    const auto now = std::chrono::steady_clock::now();
    if (g_ddg_last_init) {
        const auto el =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - g_ddg_last).count();
        if (el < min_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(min_ms - el));
        }
    }
    g_ddg_last = std::chrono::steady_clock::now();
    g_ddg_last_init = true;
}

bool is_ddg_trusted_host(std::string h) {
    for (char& c : h) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (h == "duckduckgo.com" || h == "html.duckduckgo.com") {
        return true;
    }
    static const char suf[] = ".duckduckgo.com";
    const std::size_t ls = sizeof(suf) - 1;
    if (h.size() > ls && h.compare(h.size() - ls, ls, suf) == 0) {
        return true;
    }
    return false;
}

/** 解析绝对 https URL 的 host 与 path-and-query */
bool split_https_url(const std::string& url, std::string& host, std::string& pathq) {
    const std::string pref = "https://";
    if (url.rfind(pref, 0) != 0) {
        return false;
    }
    std::size_t pos = pref.size();
    const std::size_t slash = url.find('/', pos);
    std::size_t host_end = (slash == std::string::npos) ? url.size() : slash;
    host = url.substr(pos, host_end - pos);
    const std::size_t colon = host.rfind(':');
    if (colon != std::string::npos && host.find(']') == std::string::npos) {
        host = host.substr(0, colon);
    }
    if (slash == std::string::npos) {
        pathq = "/";
    } else {
        pathq = url.substr(slash);
    }
    trim_inplace_str(host);
    return !host.empty() && is_ddg_trusted_host(host);
}

std::string merge_ddg_relative(const std::string& base_https, const std::string& loc) {
    std::string l = loc;
    trim_inplace_str(l);
    if (l.empty()) {
        return {};
    }
    if (l.rfind("https://", 0) == 0) {
        return l;
    }
    if (l.rfind("http://", 0) == 0) {
        return {};
    }
    std::string host, path_base;
    if (!split_https_url(base_https, host, path_base)) {
        return {};
    }
    if (!l.empty() && l[0] == '/') {
        return "https://" + host + l;
    }
    std::string path = path_base;
    const std::size_t q = path.find('?');
    if (q != std::string::npos) {
        path = path.substr(0, q);
    }
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        path = "/";
    } else {
        path = path.substr(0, slash + 1);
    }
    return "https://" + host + path + l;
}

} // namespace

std::vector<WebSearchHit> parse_duckduckgo_html_results(std::string_view html, int max_results) {
    std::vector<WebSearchHit> results;
    if (max_results <= 0 || html.empty()) {
        return results;
    }
    const std::string html_s(html);
    try {
        // ECMAScript (std::regex) has no reliable (?s); use [\s\S] to span newlines.
        const std::regex result_block(
            R"re(<a[^>]*class="[^"]*result__a[^"]*"[^>]*href="([^"]+)"[^>]*>([\s\S]*?)</a>[\s\S]*?<a[^>]*class="[^"]*result__snippet[^"]*"[^>]*>([\s\S]*?)</a>)re",
            std::regex::icase);
        auto begin = std::sregex_iterator(html_s.begin(), html_s.end(), result_block);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end && static_cast<int>(results.size()) < max_results; ++it) {
            WebSearchHit h;
            h.url = unwrap_ddg_redirect_url((*it)[1].str());
            h.title = strip_html_tags(html_decode_basic((*it)[2].str()));
            h.snippet = strip_html_tags(html_decode_basic((*it)[3].str()));
            trim_inplace_str(h.title);
            trim_inplace_str(h.snippet);
            results.push_back(std::move(h));
        }
    } catch (...) {
        return {};
    }
    return results;
}

json web_search_duckduckgo(const std::string& query, int max_results) {
    if (query.empty()) {
        return web_tool_error("invalid_url", "empty query");
    }
    int cap = max_results;
    if (cap <= 0) {
        cap = 10;
    }
    if (cap > 25) {
        cap = 25;
    }

    ddg_wait_rate_limit();

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    return web_tool_error("search_provider_error", "https not available");
#else
    const int timeout_ms = ddg_search_timeout_ms();
    const std::size_t max_body = ddg_max_body_bytes();
    const std::string ua = env_str_local("AGENT_WEB_USER_AGENT", "agent-framework-web-tools/1.0");

    std::string current = "https://html.duckduckgo.com/html/?q=" + url_encode_query(query);
    int redirects = 0;

    std::string body;
    bool truncated = false;
    httplib::Headers hdrs;
    hdrs.emplace("User-Agent", ua);
    hdrs.emplace("Accept", "text/html");

    while (redirects <= 10) {
        std::string host, pathq;
        if (!split_https_url(current, host, pathq)) {
            return web_tool_error("search_provider_error", "invalid ddg url");
        }

        body.clear();
        truncated = false;

        httplib::SSLClient cli(host, 443);
        {
            int ms = timeout_ms <= 0 ? 30000 : timeout_ms;
            const time_t sec = static_cast<time_t>(ms / 1000);
            const time_t usec = static_cast<time_t>((ms % 1000) * 1000);
            cli.set_connection_timeout(sec, usec);
            cli.set_read_timeout(sec, usec);
            cli.set_write_timeout(sec, usec);
        }
        cli.set_follow_location(false);

        auto on_data = [&body, max_body, &truncated](const char* data, std::size_t len) -> bool {
            if (len == 0) {
                return true;
            }
            const std::size_t room = max_body > body.size() ? (max_body - body.size()) : 0U;
            if (room == 0U) {
                truncated = true;
                return false;
            }
            const std::size_t take = std::min(room, len);
            body.append(data, take);
            if (take < len) {
                truncated = true;
                return false;
            }
            return true;
        };

        const auto res = cli.Get(pathq.c_str(), hdrs, on_data);
        if (!res) {
            return web_tool_error("timeout");
        }

        if (res->status == 301 || res->status == 302 || res->status == 303 || res->status == 307 ||
            res->status == 308) {
            if (++redirects > 10) {
                return web_tool_error("too_many_redirects");
            }
            std::string loc = res->get_header_value("Location");
            trim_inplace_str(loc);
            std::string next = merge_ddg_relative(current, loc);
            if (next.empty()) {
                return web_tool_error("search_provider_error", "bad redirect");
            }
            std::string nh;
            std::string pq;
            if (!split_https_url(next, nh, pq) || !is_ddg_trusted_host(nh)) {
                return web_tool_error("url_disallowed");
            }
            current = std::move(next);
            continue;
        }

        if (res->status != 200) {
            json e = web_tool_error("search_provider_error");
            e["error"]["status"] = res->status;
            return e;
        }
        if (truncated || body.size() > max_body) {
            return web_tool_error("body_too_large");
        }
        break;
    }

    auto hits = parse_duckduckgo_html_results(body, cap);
    json out = json::object();
    out["provider"] = "duckduckgo";
    out["query"] = query;
    out["results"] = json::array();
    for (const auto& h : hits) {
        out["results"].push_back({{"title", h.title}, {"url", h.url}, {"snippet", h.snippet}});
    }
    out["truncated"] = (static_cast<int>(hits.size()) >= cap);
    return out;
#endif
}

} // namespace agent_framework
