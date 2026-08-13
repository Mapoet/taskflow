/**
 * @file web_search_searxng.cpp
 * @brief SearXNG JSON API adapter with bounded transport and endpoint validation.
 */

#include <agent/toolbus/web_http.hpp>
#include <agent/toolbus/web_search_searxng.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <string>

namespace agent_framework {
namespace {

std::string env_string(const char* key, const char* fallback) {
    const char* value = std::getenv(key);
    return value && *value ? std::string(value) : std::string(fallback);
}

bool env_true(const char* key) {
    std::string value = env_string(key, "");
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool is_loopback_http_endpoint(const std::string& endpoint) {
    if (endpoint.rfind("http://", 0) != 0) {
        return false;
    }
    const std::size_t begin = 7;
    const std::size_t end = endpoint.find('/', begin);
    std::string authority = endpoint.substr(begin, end == std::string::npos ? std::string::npos
                                                                           : end - begin);
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        authority.resize(colon);
    }
    std::transform(authority.begin(), authority.end(), authority.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return authority == "127.0.0.1" || authority == "localhost";
}

std::string url_encode(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4U]);
            out.push_back(hex[c & 15U]);
        }
    }
    return out;
}

std::string search_url(std::string endpoint, const std::string& query) {
    while (!endpoint.empty() && endpoint.back() == '/') {
        endpoint.pop_back();
    }
    return endpoint + "/search?q=" + url_encode(query) + "&format=json";
}

} // namespace

std::vector<WebSearchHit> parse_searxng_json_results(const json& payload, int max_results) {
    std::vector<WebSearchHit> hits;
    if (max_results <= 0 || !payload.is_object() || !payload.contains("results") ||
        !payload["results"].is_array()) {
        return hits;
    }
    for (const auto& item : payload["results"]) {
        if (static_cast<int>(hits.size()) >= max_results) {
            break;
        }
        if (!item.is_object() || !item.contains("url") || !item["url"].is_string()) {
            continue;
        }
        WebSearchHit hit;
        hit.url = item["url"].get<std::string>();
        if (hit.url.rfind("http://", 0) != 0 && hit.url.rfind("https://", 0) != 0) {
            continue;
        }
        if (item.contains("title") && item["title"].is_string()) {
            hit.title = item["title"].get<std::string>();
        }
        if (item.contains("content") && item["content"].is_string()) {
            hit.snippet = item["content"].get<std::string>();
        }
        hits.push_back(std::move(hit));
    }
    return hits;
}

json web_search_searxng(const std::string& query, int max_results) {
    if (query.empty()) {
        return web_tool_error("invalid_arguments", "empty query");
    }
    const int cap = std::max(1, std::min(max_results <= 0 ? 10 : max_results, 25));
    const std::string endpoint =
        env_string("AGENT_WEB_SEARXNG_URL", "http://127.0.0.1:8080");
    if (endpoint.rfind("http://", 0) != 0 && endpoint.rfind("https://", 0) != 0) {
        return web_tool_error("search_provider_config_error", "invalid SearXNG endpoint");
    }

    WebHttpConfig cfg = load_web_http_config_from_env();
    const bool loopback_endpoint = is_loopback_http_endpoint(endpoint);
    cfg.allow_http = loopback_endpoint || env_true("AGENT_WEB_ALLOW_HTTP");
    cfg.allow_loopback = loopback_endpoint;
    // Provider credentials must never follow a redirect to a different origin.
    cfg.max_redirects = 0;
    cfg.max_body_bytes = 2U * 1024U * 1024U;

    std::map<std::string, std::string> headers{{"Accept", "application/json"}};
    const std::string api_key = env_string("AGENT_WEB_SEARXNG_API_KEY", "");
    if (!api_key.empty()) {
        std::string header = env_string("AGENT_WEB_SEARXNG_API_KEY_HEADER", "Authorization");
        headers[header] = header == "Authorization" ? "Bearer " + api_key : api_key;
    }

    const WebHttpResult response = web_http_get(search_url(endpoint, query), cfg, headers);
    if (!response.error_code.empty()) {
        json error = web_tool_error("search_provider_error", response.error_code);
        if (response.error_http_status != 0) {
            error["error"]["status"] = response.error_http_status;
        }
        return error;
    }
    json payload;
    try {
        payload = json::parse(response.body);
    } catch (...) {
        return web_tool_error("search_provider_error", "invalid SearXNG JSON response");
    }
    const auto hits = parse_searxng_json_results(payload, cap);
    json out{{"provider", "searxng"}, {"query", query}, {"results", json::array()}};
    for (const auto& hit : hits) {
        out["results"].push_back(
            {{"title", hit.title}, {"url", hit.url}, {"snippet", hit.snippet}});
    }
    out["truncated"] = static_cast<int>(hits.size()) >= cap;
    return out;
}

} // namespace agent_framework
