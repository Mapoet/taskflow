/**
 * @file web_search_searxng.hpp
 * @brief SearXNG JSON API search provider.
 */
#ifndef __AGENT_WEB_SEARCH_SEARXNG_H__
#define __AGENT_WEB_SEARCH_SEARXNG_H__

#include <agent/toolbus/web_search_ddg.hpp>

namespace agent_framework {

std::vector<WebSearchHit> parse_searxng_json_results(const json& payload, int max_results);
json web_search_searxng(const std::string& query, int max_results);

} // namespace agent_framework

#endif
