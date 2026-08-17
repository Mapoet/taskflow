/**
 * @file web_search.cpp
 * @brief Provider routing, explicit fallback, and safe content enrichment.
 */

#include <agent/toolbus/web_search.hpp>
#include <agent/toolbus/web_search_ddg.hpp>
#include <agent/toolbus/web_search_searxng.hpp>
#include <agent/toolbus/web_http.hpp>
#include <agent/toolbus/web_tools.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace agent_framework {
namespace {

std::string env_string(const char* key, const char* fallback) {
    const char* value = std::getenv(key);
    return value && *value ? std::string(value) : std::string(fallback);
}

bool bool_value(const json& args, const char* key, bool fallback) {
    if (args.contains(key) && args[key].is_boolean()) {
        return args[key].get<bool>();
    }
    std::string value = env_string("AGENT_WEB_SEARCH_FETCH_CONTENT", fallback ? "1" : "0");
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

json invoke_provider(const std::string& provider, const std::string& query, int cap) {
    if (provider == "searxng") {
        return web_search_searxng(query, cap);
    }
    if (provider == "duckduckgo") {
        return web_search_duckduckgo(query, cap);
    }
    return web_tool_error("search_provider_config_error", "unknown provider: " + provider);
}

void enrich_results(json& response, const json& args) {
    if (!response.contains("results") || !response["results"].is_array() ||
        !bool_value(args, "fetch_content", true)) {
        response["content_enrichment"] = {{"enabled", false}};
        return;
    }
    int top_k = static_cast<int>(response["results"].size());
    if (args.contains("fetch_top_k") && args["fetch_top_k"].is_number_integer()) {
        top_k = args["fetch_top_k"].get<int>();
    }
    top_k = std::max(0, std::min(top_k, static_cast<int>(response["results"].size())));
    int max_bytes = 131072;
    if (args.contains("content_max_bytes") && args["content_max_bytes"].is_number_integer()) {
        max_bytes = args["content_max_bytes"].get<int>();
    }
    max_bytes = std::max(1024, std::min(max_bytes, 262144));
    int succeeded = 0;
    int failed = 0;
    for (int i = 0; i < top_k; ++i) {
        auto& item = response["results"][static_cast<std::size_t>(i)];
        if (!item.contains("url") || !item["url"].is_string()) {
            item["content_status"] = "invalid_url";
            continue;
        }
        json fetched;
        try {
            fetched = do_web_fetch({{"url", item["url"]},
                                    {"max_bytes", max_bytes},
                                    {"extract_mode", "main_text"},
                                    {"accept", "text/html,text/plain,application/json"}});
        } catch (const std::exception& e) {
            item["content_status"] = "failed";
            item["content_error"] = {{"code", "content_enrichment_exception"},
                                     {"message", std::string(e.what()).substr(0, 256)}};
            ++failed;
            continue;
        } catch (...) {
            item["content_status"] = "failed";
            item["content_error"] = {{"code", "content_enrichment_exception"}};
            ++failed;
            continue;
        }
        if (fetched.contains("error")) {
            item["content_status"] = "failed";
            item["content_error"] = fetched["error"];
            ++failed;
            continue;
        }
        item["content_status"] = "fetched";
        item["content_type"] = fetched.value("content_type", "");
        item["content_url"] = fetched.value("final_url", item["url"].get<std::string>());
        item["content_truncated"] = fetched.value("truncated", false);
        if (fetched.contains("text")) {
            item["content"] = fetched["text"];
        } else if (fetched.contains("json")) {
            item["content"] = fetched["json"].dump();
        } else {
            item["content_status"] = "unsupported_media_type";
            ++failed;
            continue;
        }
        ++succeeded;
    }
    response["content_enrichment"] =
        {{"enabled", true}, {"attempted", top_k}, {"succeeded", succeeded}, {"failed", failed},
         {"max_bytes_per_url", max_bytes}, {"failure_isolation", "per_url"}};
}

} // namespace

json web_search(const json& args) {
    if (!args.contains("query") || !args["query"].is_string() ||
        args["query"].get<std::string>().empty()) {
        return web_tool_error("invalid_arguments", "missing query");
    }
    std::string query = args["query"].get<std::string>();
    if (args.contains("site_filter") && args["site_filter"].is_string() &&
        !args["site_filter"].get<std::string>().empty()) {
        query = "site:" + args["site_filter"].get<std::string>() + " " + query;
    }
    int cap = args.value("max_results", 10);
    cap = std::max(1, std::min(cap, 25));
    const std::string provider = args.value("provider", env_string("AGENT_WEB_SEARCH_PROVIDER", "searxng"));
    json response = invoke_provider(provider, query, cap);
    const std::string fallback = env_string("AGENT_WEB_SEARCH_FALLBACK", "none");
    if (response.contains("error") && fallback != "none" && fallback != provider) {
        const json primary_error = response["error"];
        response = invoke_provider(fallback, query, cap);
        if (!response.contains("error")) {
            response["fallback"] = {{"from", provider}, {"reason", primary_error}};
        }
    }
    if (!response.contains("error")) {
        enrich_results(response, args);
    }
    return response;
}

} // namespace agent_framework
