/**
 * @file news_sources_catalog.cpp
 * @brief 信源目录 JSON v1 解析（与 builtin-web-tools.md 一致）
 */

#include <agent/toolbus/news_sources_catalog.hpp>

#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace agent_framework {
namespace {

bool is_unreserved_query_char(unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

std::string url_encode_query_component(std::string_view s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (is_unreserved_query_char(c)) {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out += "%20";
        } else {
            static const char* H = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(H[(c >> 4) & 0xFU]);
            out.push_back(H[c & 0xFU]);
        }
    }
    return out;
}

std::string param_value_to_query_string(const json& v, std::string& err_out) {
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_number_integer()) {
        return std::to_string(v.get<long long>());
    }
    if (v.is_number_float()) {
        return std::to_string(v.get<double>());
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? "true" : "false";
    }
    if (v.is_null()) {
        return "null";
    }
    err_out = "params value must be string, number, boolean or null";
    return {};
}

bool parse_enabled_field(const json& obj, std::string& err_out, bool& enabled_out) {
    if (!obj.contains("enabled")) {
        enabled_out = true;
        return true;
    }
    if (!obj["enabled"].is_boolean()) {
        err_out = "enabled must be boolean";
        return false;
    }
    enabled_out = obj["enabled"].get<bool>();
    return true;
}

} // namespace

std::optional<NewsRssEntry> NewsSourcesCatalog::find_rss(const std::string& id) const {
    auto it = rss_.find(id);
    if (it == rss_.end() || !it->second.enabled) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<NewsApiEntry> NewsSourcesCatalog::find_api(const std::string& id) const {
    auto it = api_.find(id);
    if (it == api_.end() || !it->second.enabled) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<NewsScrapeEntry> NewsSourcesCatalog::find_scrape(const std::string& id) const {
    auto it = scrape_.find(id);
    if (it == scrape_.end() || !it->second.enabled) {
        return std::nullopt;
    }
    return it->second;
}

std::string NewsSourcesCatalog::build_api_get_url(const NewsApiEntry& e, std::string& err_out) {
    err_out.clear();
    if (e.url.empty()) {
        err_out = "empty api url";
        return {};
    }

    std::vector<std::string> keys;
    if (e.params.is_object()) {
        for (auto it = e.params.begin(); it != e.params.end(); ++it) {
            keys.push_back(it.key());
        }
        std::sort(keys.begin(), keys.end());
    } else if (!e.params.is_null() && !e.params.empty()) {
        err_out = "api params must be object";
        return {};
    }

    std::string q;
    for (const std::string& k : keys) {
        std::string val = param_value_to_query_string(e.params.at(k), err_out);
        if (!err_out.empty()) {
            return {};
        }
        if (!q.empty()) {
            q.push_back('&');
        }
        q += url_encode_query_component(k);
        q.push_back('=');
        q += url_encode_query_component(val);
    }

    if (q.empty()) {
        return e.url;
    }
    const char sep = e.url.find('?') != std::string::npos ? '&' : '?';
    return e.url + sep + q;
}

std::shared_ptr<NewsSourcesCatalog> parse_news_sources_catalog(const json& root, std::string& err_out) {
    err_out.clear();
    if (!root.is_object()) {
        err_out = "root must be object";
        return nullptr;
    }
    if (!root.contains("version") || !root["version"].is_number_integer()) {
        err_out = "missing or invalid version";
        return nullptr;
    }
    if (root["version"].get<int>() != 1) {
        err_out = "unsupported version";
        return nullptr;
    }

    auto cat = std::shared_ptr<NewsSourcesCatalog>(new NewsSourcesCatalog());
    cat->version_ = 1;

    auto parse_rss_section = [&](const char* key) -> bool {
        if (!root.contains(key)) {
            return true;
        }
        const json& sec = root[key];
        if (!sec.is_object()) {
            err_out = std::string(key) + " must be object";
            return false;
        }
        for (auto it = sec.begin(); it != sec.end(); ++it) {
            if (it.key().empty()) {
                err_out = "empty rss source_id";
                return false;
            }
            if (!it.value().is_object()) {
                err_out = "rss entry must be object";
                return false;
            }
            const json& o = it.value();
            if (!o.contains("url") || !o["url"].is_string()) {
                err_out = "rss url missing";
                return false;
            }
            NewsRssEntry e;
            e.url = o["url"].get<std::string>();
            if (!parse_enabled_field(o, err_out, e.enabled)) {
                return false;
            }
            cat->rss_[it.key()] = std::move(e);
        }
        return true;
    };

    auto parse_api_section = [&](const char* key) -> bool {
        if (!root.contains(key)) {
            return true;
        }
        const json& sec = root[key];
        if (!sec.is_object()) {
            err_out = std::string(key) + " must be object";
            return false;
        }
        for (auto it = sec.begin(); it != sec.end(); ++it) {
            if (it.key().empty()) {
                err_out = "empty api source_id";
                return false;
            }
            if (!it.value().is_object()) {
                err_out = "api entry must be object";
                return false;
            }
            const json& o = it.value();
            if (!o.contains("url") || !o["url"].is_string()) {
                err_out = "api url missing";
                return false;
            }
            NewsApiEntry e;
            e.url = o["url"].get<std::string>();
            if (!parse_enabled_field(o, err_out, e.enabled)) {
                return false;
            }
            if (o.contains("headers")) {
                if (!o["headers"].is_object()) {
                    err_out = "api headers must be object";
                    return false;
                }
                for (auto h = o["headers"].begin(); h != o["headers"].end(); ++h) {
                    if (!h.value().is_string()) {
                        err_out = "api header values must be strings";
                        return false;
                    }
                    e.headers[h.key()] = h.value().get<std::string>();
                }
            }
            if (o.contains("params")) {
                if (!o["params"].is_object()) {
                    err_out = "api params must be object";
                    return false;
                }
                for (auto p = o["params"].begin(); p != o["params"].end(); ++p) {
                    const json& v = p.value();
                    if (v.is_object() || v.is_array()) {
                        err_out = "api params must be flat";
                        return false;
                    }
                }
                e.params = o["params"];
            }
            cat->api_[it.key()] = std::move(e);
        }
        return true;
    };

    auto parse_scrape_section = [&](const char* key) -> bool {
        if (!root.contains(key)) {
            return true;
        }
        const json& sec = root[key];
        if (!sec.is_object()) {
            err_out = std::string(key) + " must be object";
            return false;
        }
        for (auto it = sec.begin(); it != sec.end(); ++it) {
            if (it.key().empty()) {
                err_out = "empty scrape source_id";
                return false;
            }
            if (!it.value().is_object()) {
                err_out = "scrape entry must be object";
                return false;
            }
            const json& o = it.value();
            if (!o.contains("url") || !o["url"].is_string()) {
                err_out = "scrape url missing";
                return false;
            }
            NewsScrapeEntry e;
            e.url = o["url"].get<std::string>();
            if (!parse_enabled_field(o, err_out, e.enabled)) {
                return false;
            }
            if (o.contains("base_url") && o["base_url"].is_string()) {
                e.base_url = o["base_url"].get<std::string>();
            }
            cat->scrape_[it.key()] = std::move(e);
        }
        return true;
    };

    if (!parse_rss_section("rss")) {
        return nullptr;
    }
    if (!parse_api_section("api")) {
        return nullptr;
    }
    if (!parse_scrape_section("scrape")) {
        return nullptr;
    }

    const bool any = !cat->rss_.empty() || !cat->api_.empty() || !cat->scrape_.empty();
    if (!any) {
        err_out = "catalog has no sources";
        return nullptr;
    }
    return cat;
}

std::shared_ptr<NewsSourcesCatalog> load_news_sources_catalog_from_file(const std::string& path,
                                                                        std::string& err_out) {
    err_out.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err_out = "cannot open file";
        return nullptr;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    json root;
    try {
        root = json::parse(ss.str());
    } catch (...) {
        err_out = "invalid_json";
        return nullptr;
    }
    return parse_news_sources_catalog(root, err_out);
}

} // namespace agent_framework
