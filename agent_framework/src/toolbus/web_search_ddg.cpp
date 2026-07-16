/**
 * @file web_search_ddg.cpp
 * @brief DuckDuckGo html.duckduckgo.com 搜索（不重定向到非 duckduckgo.com）。
 * HTTPS 请求：代理与 `web_http_get` 共用（见 `load_web_http_upstream_proxy` / `web_http.cpp`）。
 * 人机验证：若识别到 DDG 挑战页，JSON 含 ddg_challenge.open_in_browser；设 AGENT_WEB_DDG_PAUSE_ON_CHALLENGE=1
 * 可在终端暂停，验证后可选 export AGENT_WEB_DDG_COOKIE=... 再按 Enter 重试一次。
 */

#include <agent/toolbus/web_http.hpp>
#include <agent/toolbus/web_search_ddg.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <iostream>
#include <regex>
#include <string_view>
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

namespace {

std::string ddg_cookie_from_env() {
    const char* v = std::getenv("AGENT_WEB_DDG_COOKIE");
    if (v && *v) {
        return std::string(v);
    }
    return {};
}

bool ddg_pause_on_challenge_enabled() {
    const char* e = std::getenv("AGENT_WEB_DDG_PAUSE_ON_CHALLENGE");
    if (!e || !*e) {
        return false;
    }
    std::string s(e);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

bool ddg_html_looks_like_bot_challenge(std::string_view html) {
    if (html.size() < 40) {
        return false;
    }
    return html.find("Unfortunately, bots use DuckDuckGo") != std::string_view::npos ||
           html.find("anomaly-modal") != std::string_view::npos ||
           html.find("challenge-form") != std::string_view::npos ||
           html.find("js-anomaly-modal-submit") != std::string_view::npos;
}

struct DdgFetchOutcome {
    bool net_ok = false;
    json err;
    std::string body;
    std::string final_url;
    bool truncated = false;
};

DdgFetchOutcome ddg_follow_https_get(std::string current,
                                     const std::string& ua,
                                     const std::string& cookie,
                                     int timeout_ms,
                                     std::size_t max_body) {
    DdgFetchOutcome out;
    int redirects = 0;
    while (redirects <= 10) {
        std::string host, pathq;
        if (!split_https_url(current, host, pathq)) {
            out.err = web_tool_error("search_provider_error", "invalid ddg url");
            return out;
        }

        std::string body;
        bool truncated = false;
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

        WebHttpUpstreamProxy upstream_proxy;
        if (load_web_http_upstream_proxy(upstream_proxy)) {
            cli.set_proxy(upstream_proxy.host.c_str(), upstream_proxy.port);
            if (!upstream_proxy.user.empty()) {
                cli.set_proxy_basic_auth(upstream_proxy.user.c_str(), upstream_proxy.pass.c_str());
            }
        }

        httplib::Headers hdrs;
        hdrs.emplace("User-Agent", ua);
        hdrs.emplace("Accept", "text/html");
        if (!cookie.empty()) {
            hdrs.emplace("Cookie", cookie);
        }

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
            out.err = web_tool_error("timeout");
            return out;
        }

        if (res->status == 301 || res->status == 302 || res->status == 303 || res->status == 307 ||
            res->status == 308) {
            if (++redirects > 10) {
                out.err = web_tool_error("too_many_redirects");
                return out;
            }
            std::string loc = res->get_header_value("Location");
            trim_inplace_str(loc);
            std::string next = merge_ddg_relative(current, loc);
            if (next.empty()) {
                out.err = web_tool_error("search_provider_error", "bad redirect");
                return out;
            }
            std::string nh;
            std::string pq;
            if (!split_https_url(next, nh, pq) || !is_ddg_trusted_host(nh)) {
                out.err = web_tool_error("url_disallowed");
                return out;
            }
            current = std::move(next);
            continue;
        }

        if (res->status < 200 || res->status >= 300) {
            out.err = web_tool_error("search_provider_error");
            out.err["error"]["status"] = res->status;
            return out;
        }
        if (truncated || body.size() > max_body) {
            out.err = web_tool_error("body_too_large");
            return out;
        }
        out.net_ok = true;
        out.body = std::move(body);
        out.final_url = current;
        out.truncated = truncated;
        return out;
    }
    out.err = web_tool_error("too_many_redirects");
    return out;
}

} // namespace

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

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    return web_tool_error("search_provider_error", "https not available");
#else
    const int timeout_ms = ddg_search_timeout_ms();
    const std::size_t max_body = ddg_max_body_bytes();
    const std::string ua = env_str_local("AGENT_WEB_USER_AGENT", "agent-framework-web-tools/1.0");
    const std::string start_url = "https://html.duckduckgo.com/html/?q=" + url_encode_query(query);

    ddg_wait_rate_limit();
    std::string cookie = ddg_cookie_from_env();
    auto fr = ddg_follow_https_get(start_url, ua, cookie, timeout_ms, max_body);
    if (!fr.net_ok) {
        return fr.err;
    }

    auto hits = parse_duckduckgo_html_results(fr.body, cap);
    bool challenge = hits.empty() && ddg_html_looks_like_bot_challenge(fr.body);
    bool did_interactive_retry = false;

    if (challenge && ddg_pause_on_challenge_enabled()) {
        did_interactive_retry = true;
        std::cerr << "[web_search_ddg] DuckDuckGo 疑似人机验证页。请在浏览器打开:\n  " << start_url
                  << "\n\n可选: 完成验证后从开发者工具复制 html.duckduckgo.com 请求的 Cookie，执行:\n"
                     "  export AGENT_WEB_DDG_COOKIE='...'\n"
                     "然后回到此终端按 Enter（将用当前 AGENT_WEB_DDG_COOKIE 重试一次；不设则空 Cookie 重试）。\n";
        std::string line;
        std::getline(std::cin, line);
        (void)line;
        ddg_wait_rate_limit();
        cookie = ddg_cookie_from_env();
        fr = ddg_follow_https_get(start_url, ua, cookie, timeout_ms, max_body);
        if (!fr.net_ok) {
            return fr.err;
        }
        hits = parse_duckduckgo_html_results(fr.body, cap);
        challenge = hits.empty() && ddg_html_looks_like_bot_challenge(fr.body);
    }

    json out = json::object();
    out["provider"] = "duckduckgo";
    out["query"] = query;
    out["results"] = json::array();
    for (const auto& h : hits) {
        out["results"].push_back({{"title", h.title}, {"url", h.url}, {"snippet", h.snippet}});
    }
    out["truncated"] = (static_cast<int>(hits.size()) >= cap);

    if (challenge) {
        json ch;
        ch["detected"] = true;
        ch["open_in_browser"] = start_url;
        if (did_interactive_retry) {
            ch["interactive_retry"] = true;
            ch["note"] =
                "已交互重试一次；若 results 仍为空，请确认 Cookie 与浏览器会话一致或稍后重试。";
        }
        out["ddg_challenge"] = std::move(ch);
    }
    return out;
#endif
}

} // namespace agent_framework
