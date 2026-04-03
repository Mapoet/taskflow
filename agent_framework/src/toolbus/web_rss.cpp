/**
 * @file web_rss.cpp
 * @brief web_rss_feed：RSS 2.0 / Atom 子集解析（无完整 XML 库）
 */

#ifdef __linux__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include <agent/web_http.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <map>
#include <optional>
#include <sstream>
#include <string>

#ifdef __linux__
#include <time.h>
#endif

namespace agent_framework {
namespace {

void trim(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
}

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string strip_cdata(const std::string& s) {
    std::string t = s;
    const std::string a = "<![CDATA[";
    const std::string b = "]]>";
    if (t.size() >= a.size() + b.size() && t.compare(0, a.size(), a) == 0 &&
        t.compare(t.size() - b.size(), b.size(), b) == 0) {
        t = t.substr(a.size(), t.size() - a.size() - b.size());
    }
    return t;
}

/** 在 xml 中从 from 起找局部 tag 的文本（首个匹配） */
std::optional<std::string> xml_inner(const std::string& xml, const std::string& tag,
                                     std::size_t from = 0) {
    const std::string open = "<" + tag;
    const std::size_t p = xml.find(open, from);
    if (p == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t gt = xml.find('>', p);
    if (gt == std::string::npos) {
        return std::nullopt;
    }
    if (gt > p + open.size()) {
        const std::string mid = xml.substr(p + open.size(), gt - (p + open.size()));
        if (!mid.empty() && mid[0] != ' ' && mid[0] != '>' && mid != "/") {
            return std::nullopt;
        }
    }
    const std::string close = "</" + tag + ">";
    const std::size_t c = xml.find(close, gt + 1);
    if (c == std::string::npos) {
        return std::nullopt;
    }
    std::string inner = xml.substr(gt + 1, c - gt - 1);
    trim(inner);
    return strip_cdata(inner);
}

std::optional<std::string> atom_link_href(const std::string& entry_chunk) {
    const std::string needle = "link";
    std::size_t p = 0;
    while ((p = entry_chunk.find("<link", p)) != std::string::npos) {
        const std::size_t gt = entry_chunk.find('>', p);
        if (gt == std::string::npos) {
            break;
        }
        const std::string attrs = entry_chunk.substr(p, gt - p);
        if (attrs.find("href=\"") != std::string::npos) {
            const std::size_t h = attrs.find("href=\"");
            const std::size_t q = h + 6;
            const std::size_t e = attrs.find('"', q);
            if (e != std::string::npos) {
                return attrs.substr(q, e - q);
            }
        }
        p = gt + 1;
    }
    return std::nullopt;
}

#ifdef __linux__
std::optional<std::time_t> parse_iso_or_rfc(const std::string& raw) {
    struct tm tm {};
    const char* rest = strptime(raw.c_str(), "%Y-%m-%dT%H:%M:%S", &tm);
    if (rest != nullptr) {
        return timegm(&tm);
    }
    struct tm tm2 {};
    rest = strptime(raw.c_str(), "%a, %d %b %Y %H:%M:%S", &tm2);
    if (rest != nullptr) {
        return timegm(&tm2);
    }
    struct tm tm3 {};
    rest = strptime(raw.c_str(), "%d %b %Y %H:%M:%S", &tm3);
    if (rest != nullptr) {
        return timegm(&tm3);
    }
    return std::nullopt;
}
#else
std::optional<std::time_t> parse_iso_or_rfc(const std::string& /*raw*/) {
    return std::nullopt;
}
#endif

bool keywords_match(const std::string& title, const std::string& summary,
                    const json& keywords_j) {
    if (!keywords_j.is_array() || keywords_j.empty()) {
        return true;
    }
    const std::string blob = to_lower(title + " " + summary);
    for (const auto& kw : keywords_j) {
        if (!kw.is_string()) {
            continue;
        }
        const std::string k = to_lower(kw.get<std::string>());
        if (k.empty()) {
            continue;
        }
        if (blob.find(k) != std::string::npos) {
            return true;
        }
    }
    return false;
}

json parse_rss_items(const std::string& xml, int max_entries, int max_age_hours,
                      bool skip_keyword_filter, const json& keywords) {
    json entries = json::array();
    std::size_t pos = 0;
    const std::time_t now = std::time(nullptr);
    const std::time_t cutoff =
        (max_age_hours > 0) ? (now - static_cast<std::time_t>(max_age_hours) * 3600) : 0;

    while (static_cast<int>(entries.size()) < max_entries) {
        const std::size_t it = xml.find("<item", pos);
        if (it == std::string::npos) {
            break;
        }
        const std::size_t it_end = xml.find("</item>", it);
        if (it_end == std::string::npos) {
            break;
        }
        const std::string chunk = xml.substr(it, it_end + 7 - it);
        pos = it_end + 7;

        auto title = xml_inner(chunk, "title", 0);
        auto link_o = xml_inner(chunk, "link", 0);
        auto desc = xml_inner(chunk, "description", 0);
        if (!desc) {
            desc = xml_inner(chunk, "summary", 0);
        }
        std::string title_s = title.value_or("");
        std::string link_s = link_o.value_or("");
        std::string sum_s = desc.value_or("");

        if (max_age_hours > 0) {
            auto pub = xml_inner(chunk, "pubDate", 0);
            if (pub) {
                const auto tt = parse_iso_or_rfc(*pub);
                if (tt.has_value() && *tt < cutoff) {
                    continue;
                }
            }
        }
        if (!skip_keyword_filter && !keywords_match(title_s, sum_s, keywords)) {
            continue;
        }

        json e = json::object();
        e["title"] = title_s;
        e["url"] = link_s;
        e["summary"] = sum_s;
        if (auto pub = xml_inner(chunk, "pubDate", 0)) {
            e["published"] = *pub;
        }
        e["source_type"] = "rss";
        entries.push_back(std::move(e));
    }
    return entries;
}

json parse_atom_entries(const std::string& xml, int max_entries, int max_age_hours,
                        bool skip_keyword_filter, const json& keywords) {
    json entries = json::array();
    std::size_t pos = 0;
    const std::time_t now = std::time(nullptr);
    const std::time_t cutoff =
        (max_age_hours > 0) ? (now - static_cast<std::time_t>(max_age_hours) * 3600) : 0;

    while (static_cast<int>(entries.size()) < max_entries) {
        const std::size_t it = xml.find("<entry", pos);
        if (it == std::string::npos) {
            break;
        }
        const std::size_t it_end = xml.find("</entry>", it);
        if (it_end == std::string::npos) {
            break;
        }
        const std::string chunk = xml.substr(it, it_end + 8 - it);
        pos = it_end + 8;

        auto title = xml_inner(chunk, "title", 0);
        auto sum = xml_inner(chunk, "summary", 0);
        if (!sum) {
            sum = xml_inner(chunk, "content", 0);
        }
        std::string title_s = title.value_or("");
        std::string link_s;
        if (auto href = atom_link_href(chunk)) {
            link_s = *href;
        }
        std::string sum_s = sum.value_or("");

        if (max_age_hours > 0) {
            auto upd = xml_inner(chunk, "updated", 0);
            if (!upd) {
                upd = xml_inner(chunk, "published", 0);
            }
            if (upd) {
                const auto tt = parse_iso_or_rfc(*upd);
                if (tt.has_value() && *tt < cutoff) {
                    continue;
                }
            }
        }
        if (!skip_keyword_filter && !keywords_match(title_s, sum_s, keywords)) {
            continue;
        }

        json e = json::object();
        e["title"] = title_s;
        e["url"] = link_s;
        e["summary"] = sum_s;
        if (auto u = xml_inner(chunk, "updated", 0)) {
            e["published"] = *u;
        } else if (auto p = xml_inner(chunk, "published", 0)) {
            e["published"] = *p;
        }
        e["source_type"] = "rss";
        entries.push_back(std::move(e));
    }
    return entries;
}

json do_rss_feed(const json& j) {
    if (!j.contains("feed_url") || !j["feed_url"].is_string()) {
        return web_tool_error("invalid_url", "missing feed_url");
    }
    const std::string feed_url = j["feed_url"].get<std::string>();
    int max_entries = 15;
    if (j.contains("max_entries") && j["max_entries"].is_number_integer()) {
        max_entries = j["max_entries"].get<int>();
    }
    if (max_entries <= 0) {
        max_entries = 15;
    }
    if (max_entries > 100) {
        max_entries = 100;
    }
    int max_age_hours = 72;
    if (j.contains("max_age_hours") && j["max_age_hours"].is_number_integer()) {
        max_age_hours = j["max_age_hours"].get<int>();
    }
    bool skip_kw = false;
    if (j.contains("skip_keyword_filter") && j["skip_keyword_filter"].is_boolean()) {
        skip_kw = j["skip_keyword_filter"].get<bool>();
    }
    json keywords = json::array();
    if (j.contains("keywords") && j["keywords"].is_array()) {
        keywords = j["keywords"];
    }

    WebHttpConfig cfg = load_web_http_config_from_env();
    std::map<std::string, std::string> extra{{"Accept", "application/rss+xml, application/atom+xml, application/xml, text/xml, */*"}};
    const auto hres = web_http_get(feed_url, cfg, extra);
    if (!hres.error_code.empty()) {
        json e = json{{"error", json{{"code", hres.error_code}}}};
        if (hres.error_http_status != 0) {
            e["error"]["status"] = hres.error_http_status;
        }
        return e;
    }

    const std::string& x = hres.body;
    json entries;
    if (x.find("<feed") != std::string::npos && x.find("<entry") != std::string::npos) {
        entries = parse_atom_entries(x, max_entries, max_age_hours, skip_kw, keywords);
    } else if (x.find("<rss") != std::string::npos || x.find("<item>") != std::string::npos ||
               x.find("<item ") != std::string::npos) {
        entries = parse_rss_items(x, max_entries, max_age_hours, skip_kw, keywords);
    } else {
        return web_tool_error("rss_parse_error");
    }

    json out = json::object();
    out["feed_url"] = feed_url;
    out["entries"] = std::move(entries);
    return out;
}

} // namespace

json web_rss_feed_invoke(const json& j) {
    return do_rss_feed(j);
}

} // namespace agent_framework
