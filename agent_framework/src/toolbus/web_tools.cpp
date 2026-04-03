/**
 * @file web_tools.cpp
 * @brief 注册 web_search / web_fetch / web_rss_feed / web_fetch_archive（AGENT_WEB_ENABLE）
 *
 * 实施契约（Milestone 1）摘要：
 * - AGENT_WEB_ENABLE 为真且编译启用 CPPHTTPLIB_OPENSSL_SUPPORT 时注册（HTTPS 工具链）。
 * - web_fetch：超体截断并 truncated；http 仅当 AGENT_WEB_ALLOW_HTTP。
 * - web_search：DuckDuckGo HTML，仅 *.duckduckgo.com 重定向。
 */

#include <agent/web_search_ddg.hpp>
#include <agent/web_tools.hpp>

#include <cctype>
#include <cstdlib>
#include <iostream>

namespace agent_framework {
namespace {

bool env_truthy(const char* v) {
    if (!v || !*v) {
        return false;
    }
    std::string s(v);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

bool web_register_enabled() {
    return env_truthy(std::getenv("AGENT_WEB_ENABLE"));
}

bool log_web_debug() {
    const char* e = std::getenv("AGENT_LOG_LEVEL");
    if (!e || !*e) {
        return false;
    }
    std::string s(e);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s == "debug";
}

json web_search_invoke(const json& j) {
    std::string q;
    if (j.contains("query") && j["query"].is_string()) {
        q = j["query"].get<std::string>();
    }
    int max_results = 10;
    if (j.contains("max_results") && j["max_results"].is_number_integer()) {
        max_results = j["max_results"].get<int>();
    }
    if (j.contains("site_filter") && j["site_filter"].is_string()) {
        const std::string sf = j["site_filter"].get<std::string>();
        if (!sf.empty()) {
            q = "site:" + sf + " " + q;
        }
    }
    return web_search_duckduckgo(q, max_results);
}

} // namespace

void register_builtin_web_tools_if_configured(ToolBus& bus) {
    if (bus.get_tool_info("web_search").has_value()) {
        return;
    }
    if (!web_register_enabled()) {
        if (log_web_debug()) {
            std::clog << "[web_tools] skip: AGENT_WEB_ENABLE not set\n";
        }
        return;
    }
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    if (log_web_debug()) {
        std::clog << "[web_tools] skip: CPPHTTPLIB_OPENSSL_SUPPORT off\n";
    }
    return;
#else
    {
        ToolMeta meta;
        meta.name = "web_search";
        meta.description =
            "Search the web via DuckDuckGo HTML (html.duckduckgo.com). Returns title, url, snippet per "
            "result; does not fetch full pages.";
        meta.schema = json::parse(R"({
            "type": "object",
            "properties": {
                "query": {"type": "string"},
                "max_results": {"type": "integer"},
                "site_filter": {"type": "string"}
            },
            "required": ["query"]
        })");
        bus.register_local_tool(
            "web_search", [](const json& args) { return web_search_invoke(args); }, meta);
    }
    {
        ToolMeta meta;
        meta.name = "web_fetch";
        meta.description =
            "HTTP(S) GET a single URL with SSRF checks; returns json/text/html or binary hex preview. "
            "Respects AGENT_WEB_ALLOW_HOSTS / deny-private-IP policy.";
        meta.schema = json::parse(R"({
            "type": "object",
            "properties": {
                "url": {"type": "string"},
                "max_bytes": {"type": "integer"},
                "accept": {"type": "string"},
                "extract_mode": {"type": "string"}
            },
            "required": ["url"]
        })");
        bus.register_local_tool(
            "web_fetch", [](const json& args) { return web_fetch_invoke(args); }, meta);
    }
    {
        ToolMeta meta;
        meta.name = "web_rss_feed";
        meta.description =
            "Fetch and parse RSS 2.0 or Atom (subset). Optional keywords filter and max_age_hours.";
        meta.schema = json::parse(R"({
            "type": "object",
            "properties": {
                "feed_url": {"type": "string"},
                "max_entries": {"type": "integer"},
                "max_age_hours": {"type": "integer"},
                "keywords": {"type": "array", "items": {"type": "string"}},
                "skip_keyword_filter": {"type": "boolean"}
            },
            "required": ["feed_url"]
        })");
        bus.register_local_tool(
            "web_rss_feed", [](const json& args) { return web_rss_feed_invoke(args); }, meta);
    }
    {
        ToolMeta meta;
        meta.name = "web_fetch_archive";
        meta.description =
            "Download a .zip URL and extract under AGENT_FS_ROOT/AGENT_WEB_EXTRACT_SUBDIR/<jobid>/ "
            "(Zip Slip guarded; needs zlib for deflate).";
        meta.schema = json::parse(R"({
            "type": "object",
            "properties": {
                "url": {"type": "string"},
                "subdir": {"type": "string"}
            },
            "required": ["url"]
        })");
        bus.register_local_tool(
            "web_fetch_archive", [](const json& args) { return web_fetch_archive_invoke(args); },
            meta);
    }
#endif
}

} // namespace agent_framework
