/**
 * @file news_sources_tool.cpp
 * @brief web_configured_source：AGENT_NEWS_SOURCES_JSON + 目录 v1
 */

#include <agent/news_sources_catalog.hpp>
#include <agent/news_sources_tool.hpp>
#include <agent/web_http.hpp>
#include <agent/web_tools.hpp>

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <mutex>

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

std::shared_ptr<const NewsSourcesCatalog> load_catalog_once() {
    static std::shared_ptr<const NewsSourcesCatalog> cached;
    static std::once_flag once;
    std::call_once(once, [&] {
        const char* path = std::getenv("AGENT_NEWS_SOURCES_JSON");
        if (path == nullptr || path[0] == '\0') {
            if (log_web_debug()) {
                std::clog << "[news_sources] skip: AGENT_NEWS_SOURCES_JSON unset\n";
            }
            return;
        }
        std::string err;
        auto c = load_news_sources_catalog_from_file(path, err);
        if (!c) {
            if (log_web_debug()) {
                std::clog << "[news_sources] load failed: " << err << "\n";
            }
            return;
        }
        if (log_web_debug()) {
            std::clog << "[news_sources] loaded " << path << " rss=" << c->rss().size()
                      << " api=" << c->api().size() << " scrape=" << c->scrape().size() << "\n";
        }
        cached = std::move(c);
    });
    return cached;
}

json web_configured_source_invoke(std::shared_ptr<const NewsSourcesCatalog> cat, const json& j) {
    if (!cat) {
        return web_tool_error("news_sources_not_configured");
    }
    if (!j.contains("kind") || !j["kind"].is_string()) {
        return web_tool_error("invalid_arguments", "missing kind");
    }
    if (!j.contains("source_id") || !j["source_id"].is_string()) {
        return web_tool_error("invalid_arguments", "missing source_id");
    }
    const std::string kind = j["kind"].get<std::string>();
    const std::string source_id = j["source_id"].get<std::string>();

    if (kind == "rss") {
        auto it = cat->rss().find(source_id);
        if (it == cat->rss().end()) {
            return web_tool_error("unknown_source");
        }
        if (!it->second.enabled) {
            return web_tool_error("source_disabled");
        }
        json inner = json{{"feed_url", it->second.url}};
        if (j.contains("max_entries") && j["max_entries"].is_number_integer()) {
            inner["max_entries"] = j["max_entries"];
        }
        if (j.contains("max_age_hours") && j["max_age_hours"].is_number_integer()) {
            inner["max_age_hours"] = j["max_age_hours"];
        }
        if (j.contains("keywords") && j["keywords"].is_array()) {
            inner["keywords"] = j["keywords"];
        }
        if (j.contains("skip_keyword_filter") && j["skip_keyword_filter"].is_boolean()) {
            inner["skip_keyword_filter"] = j["skip_keyword_filter"];
        }
        json out = web_rss_feed_invoke(inner);
        if (!out.contains("error")) {
            out["catalog_kind"] = "rss";
            out["catalog_source_id"] = source_id;
        }
        return out;
    }

    if (kind == "api") {
        auto it = cat->api().find(source_id);
        if (it == cat->api().end()) {
            return web_tool_error("unknown_source");
        }
        if (!it->second.enabled) {
            return web_tool_error("source_disabled");
        }
        std::string be;
        const std::string url = NewsSourcesCatalog::build_api_get_url(it->second, be);
        if (!be.empty()) {
            return web_tool_error("news_sources_invalid", be);
        }
        if (url.empty()) {
            return web_tool_error("news_sources_invalid", "empty url");
        }
        json inner = json{{"url", url}, {"accept", "application/json, */*"}};
        if (j.contains("max_bytes") && j["max_bytes"].is_number_integer()) {
            inner["max_bytes"] = j["max_bytes"];
        }
        if (!it->second.headers.empty()) {
            json hdr = json::object();
            for (const auto& kv : it->second.headers) {
                hdr[kv.first] = kv.second;
            }
            inner["headers"] = std::move(hdr);
        }
        json out = do_web_fetch(inner);
        if (!out.contains("error")) {
            out["catalog_kind"] = "api";
            out["catalog_source_id"] = source_id;
        }
        return out;
    }

    if (kind == "scrape") {
        auto it = cat->scrape().find(source_id);
        if (it == cat->scrape().end()) {
            return web_tool_error("unknown_source");
        }
        if (!it->second.enabled) {
            return web_tool_error("source_disabled");
        }
        json inner = json{{"url", it->second.url}, {"accept", "text/html, */*"}};
        if (j.contains("max_bytes") && j["max_bytes"].is_number_integer()) {
            inner["max_bytes"] = j["max_bytes"];
        }
        json out = do_web_fetch(inner);
        if (!out.contains("error")) {
            out["catalog_kind"] = "scrape";
            out["catalog_source_id"] = source_id;
            if (it->second.base_url.has_value()) {
                out["catalog_base_url"] = *it->second.base_url;
            }
        }
        return out;
    }

    return web_tool_error("invalid_arguments", "kind must be rss, api or scrape");
}

} // namespace

void register_web_configured_source_if_configured(ToolBus& bus) {
    if (bus.get_tool_info("web_configured_source").has_value()) {
        return;
    }
    if (!web_register_enabled()) {
        if (log_web_debug()) {
            std::clog << "[news_sources] skip: AGENT_WEB_ENABLE not set\n";
        }
        return;
    }
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    if (log_web_debug()) {
        std::clog << "[news_sources] skip: CPPHTTPLIB_OPENSSL_SUPPORT off\n";
    }
    return;
#else
    std::shared_ptr<const NewsSourcesCatalog> cat = load_catalog_once();
    if (!cat) {
        return;
    }
    ToolMeta meta;
    meta.name = "web_configured_source";
    meta.description =
        "Fetch a pre-configured news/source entry from AGENT_NEWS_SOURCES_JSON (rss -> web_rss_feed, "
        "api/scrape -> web_fetch). Use kind + source_id matching the catalog keys.";
    meta.schema = json::parse(R"({
            "type": "object",
            "properties": {
                "kind": {"type": "string", "description": "rss | api | scrape"},
                "source_id": {"type": "string"},
                "max_entries": {"type": "integer"},
                "max_age_hours": {"type": "integer"},
                "keywords": {"type": "array", "items": {"type": "string"}},
                "skip_keyword_filter": {"type": "boolean"},
                "max_bytes": {"type": "integer"}
            },
            "required": ["kind", "source_id"]
        })");
    bus.register_local_tool(
        "web_configured_source",
        [cat](const json& args) { return web_configured_source_invoke(cat, args); }, meta);
#endif
}

} // namespace agent_framework
