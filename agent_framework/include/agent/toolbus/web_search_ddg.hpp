/**
 * @file web_search_ddg.hpp
 * @brief DuckDuckGo HTML 搜索解析（供单测与 web_search 工具复用）
 */
#ifndef __AGENT_WEB_SEARCH_DDG_H__
#define __AGENT_WEB_SEARCH_DDG_H__

#include <agent/core/types.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace agent_framework {

struct WebSearchHit {
    std::string title;
    std::string url;
    std::string snippet;
};

/**
 * @brief 从 DuckDuckGo HTML 结果页提取条目（轻量正则；页面改版可能导致空结果）。
 */
std::vector<WebSearchHit> parse_duckduckgo_html_results(std::string_view html, int max_results);

/**
 * @brief 执行 DuckDuckGo GET 并解析；失败返回带 error.code 的 JSON，成功形态见 builtin-web-tools.md。
 */
json web_search_duckduckgo(const std::string& query, int max_results);

} // namespace agent_framework

#endif // __AGENT_WEB_SEARCH_DDG_H__
