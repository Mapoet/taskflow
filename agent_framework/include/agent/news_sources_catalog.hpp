/**
 * @file news_sources_catalog.hpp
 * @brief 信源目录 JSON v1：加载、校验、解析 rss/api/scrape 条目
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-04
 */
#ifndef __AGENT_NEWS_SOURCES_CATALOG_H__
#define __AGENT_NEWS_SOURCES_CATALOG_H__

#include "types.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>

namespace agent_framework {

struct NewsRssEntry {
    std::string url;
    bool enabled = true;
};

struct NewsApiEntry {
    std::string url;
    std::map<std::string, std::string> headers;
    json params = json::object();
    bool enabled = true;
};

struct NewsScrapeEntry {
    std::string url;
    std::optional<std::string> base_url;
    bool enabled = true;
};

/**
 * @brief 只读信源目录（version 1）。
 */
class NewsSourcesCatalog {
public:
    NewsSourcesCatalog(const NewsSourcesCatalog&) = delete;
    NewsSourcesCatalog& operator=(const NewsSourcesCatalog&) = delete;

    int version() const {
        return version_;
    }
    const std::map<std::string, NewsRssEntry>& rss() const {
        return rss_;
    }
    const std::map<std::string, NewsApiEntry>& api() const {
        return api_;
    }
    const std::map<std::string, NewsScrapeEntry>& scrape() const {
        return scrape_;
    }

    std::optional<NewsRssEntry> find_rss(const std::string& id) const;
    std::optional<NewsApiEntry> find_api(const std::string& id) const;
    std::optional<NewsScrapeEntry> find_scrape(const std::string& id) const;

    /** @brief 将 api.url 与 params（键按字典序）拼为 GET URL */
    static std::string build_api_get_url(const NewsApiEntry& e, std::string& err_out);

private:
    NewsSourcesCatalog() = default;
    friend std::shared_ptr<NewsSourcesCatalog> parse_news_sources_catalog(const json& root,
                                                                         std::string& err_out);
    int version_ = 1;
    std::map<std::string, NewsRssEntry> rss_;
    std::map<std::string, NewsApiEntry> api_;
    std::map<std::string, NewsScrapeEntry> scrape_;
};

/**
 * @brief 从已解析 JSON 构建目录；失败时 err_out 为人类可读短因，返回 nullptr。
 */
std::shared_ptr<NewsSourcesCatalog> parse_news_sources_catalog(const json& root, std::string& err_out);

/**
 * @brief 读取文件 UTF-8 为 JSON 再 parse；IO/JSON 失败时 err_out 非空。
 */
std::shared_ptr<NewsSourcesCatalog> load_news_sources_catalog_from_file(const std::string& path,
                                                                        std::string& err_out);

} // namespace agent_framework

#endif // __AGENT_NEWS_SOURCES_CATALOG_H__
