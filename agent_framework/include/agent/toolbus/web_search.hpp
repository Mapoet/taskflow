/**
 * @file web_search.hpp
 * @brief Provider-neutral web search routing and optional page-content enrichment.
 */
#ifndef __AGENT_WEB_SEARCH_H__
#define __AGENT_WEB_SEARCH_H__

#include <agent/core/types.hpp>

namespace agent_framework {

/** Resolve provider, execute search, and enrich result URLs through web_fetch. */
json web_search(const json& args);

} // namespace agent_framework

#endif
