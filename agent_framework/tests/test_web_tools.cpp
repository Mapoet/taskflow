/**
 * @file test_web_tools.cpp
 * @brief web_* 内建工具：DDG 解析 fixture、mock HTTP、SSRF、RSS、ZIP 解压（无公网）；默认在 clog 打印结果摘要。
 * 在线样例（可选）：设 AGENT_TEST_WEB_LIVE=1 在全部离线用例通过后调用 web_search，请求与
 * https://html.duckduckgo.com/html/?q=%E8%A5%BF%E5%AE%89 相同形态的 HTML 端点；DDG 常对自动化流量返回人机验证（见外站说明），
 * 默认不因空结果/错误失败。AGENT_TEST_WEB_LIVE_STRICT=1 时要求每次调用成功且 results 非空。
 * web_search 遇人机验证时 JSON 含 ddg_challenge.open_in_browser；交互重试：AGENT_WEB_DDG_PAUSE_ON_CHALLENGE=1、AGENT_WEB_DDG_COOKIE。
 */

#include <agent/toolbus/fs_tools.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/toolbus/web_search_ddg.hpp>
#include <agent/toolbus/web_search.hpp>
#include <agent/toolbus/web_search_searxng.hpp>
#include <agent/toolbus/web_tools.hpp>

#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#if __has_include(<httplib/httplib.hpp>)
#include <httplib/httplib.hpp>
#elif __has_include(<httplib.hpp>)
#include <httplib.hpp>
#endif

#ifndef AGENT_TEST_WEB_DATA_DIR
#define AGENT_TEST_WEB_DATA_DIR "tests/data"
#endif

namespace {

// 默认将解析/工具返回摘要打到 stderr（clog）；设 AGENT_TEST_WEB_QUIET=1 可关闭（便于 CTest 静默跑）。
bool web_test_show_query_results() {
    const char* e = std::getenv("AGENT_TEST_WEB_QUIET");
    if (!e || !*e) {
        return true;
    }
    std::string s(e);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return !(s == "1" || s == "true" || s == "yes" || s == "on");
}

bool env_truthy_cstr(const char* v) {
    if (!v || !*v) {
        return false;
    }
    std::string s(v);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

void clog_tool_json_for_test(std::string_view tag, const json& r) {
    if (!web_test_show_query_results()) {
        return;
    }
    json j = r;
    if (j.contains("binary_preview_hex") && j["binary_preview_hex"].is_string()) {
        const auto& hx = j["binary_preview_hex"].get_ref<const std::string&>();
        j["binary_preview_hex"] = "<omitted; " + std::to_string(hx.size()) + " hex chars>";
    }
    static const char* long_keys[] = {"html", "text"};
    for (const char* key : long_keys) {
        if (!j.contains(key) || !j[key].is_string()) {
            continue;
        }
        const auto& t = j[key].get_ref<const std::string&>();
        if (t.size() > 400) {
            j[key] = t.substr(0, 400) + "... <truncated, total " + std::to_string(t.size()) + " bytes>";
        }
    }
    std::clog << "[test_web_tools] " << tag << ":\n" << j.dump(2) << "\n";
}

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/**
 * 对线上 DuckDuckGo HTML 搜索做抽样（与 web_search 相同：GET
 * https://html.duckduckgo.com/html/?q=... ，由 web_search_ddg.cpp 构造）。
 */
void run_live_ddg_search_samples(agent_framework::ToolBus& bus) {
    if (!env_truthy_cstr(std::getenv("AGENT_TEST_WEB_LIVE"))) {
        return;
    }
    if (!std::getenv("AGENT_WEB_SEARCH_TIMEOUT_MS")) {
        (void)::setenv("AGENT_WEB_SEARCH_TIMEOUT_MS", "12000", 0);
    }
    const bool strict = env_truthy_cstr(std::getenv("AGENT_TEST_WEB_LIVE_STRICT"));
    std::clog << "[test_web_tools] LIVE: AGENT_TEST_WEB_LIVE=1 — 正在请求 DuckDuckGo HTML（可能被识别为 bot）\n";

    static const struct {
        const char* tag;
        json args;
    } k_cases[] = {
        {"q_xi_an", json{{"query", std::string("\xE8\xA5\xBF\xE5\xAE\x89", 6)}, {"provider", "duckduckgo"}, {"fetch_content", false}, {"max_results", 6}}},
        {"en_taskflow", json{{"query", "taskflow cpp parallel"}, {"provider", "duckduckgo"}, {"fetch_content", false}, {"max_results", 6}}},
        {"site_wikipedia_gnu",
         json{{"query", "GNU"}, {"provider", "duckduckgo"}, {"fetch_content", false}, {"site_filter", "wikipedia.org"}, {"max_results", 4}}},
        {"latin_short", json{{"query", "openstreetmap"}, {"provider", "duckduckgo"}, {"fetch_content", false}, {"max_results", 5}}},
    };

    for (const auto& c : k_cases) {
        json r = bus.call_tool("web_search", c.args).get();
        clog_tool_json_for_test(std::string("web_search LIVE [") + c.tag + "]", r);

        const bool has_err = r.contains("error");
        const bool has_results =
            r.contains("results") && r["results"].is_array() && !r["results"].empty();

        if (strict) {
            assert(!has_err && "AGENT_TEST_WEB_LIVE_STRICT: web_search returned error");
            assert(has_results && "AGENT_TEST_WEB_LIVE_STRICT: expected non-empty results");
            assert(r["results"][0].contains("url"));
            assert(r["results"][0]["url"].is_string());
            assert(!r["results"][0]["url"].get<std::string>().empty());
            continue;
        }
        if (has_err) {
            std::clog << "[test_web_tools] LIVE: " << c.tag << " error (network/HTTP/provider)\n";
            continue;
        }
        if (!has_results) {
            std::clog << "[test_web_tools] LIVE: " << c.tag
                      << " empty results (DDG 人机验证或 HTML 改版；浏览器访问示例: "
                         "https://html.duckduckgo.com/html/?q=%E8%A5%BF%E5%AE%89 )\n";
        }
    }
}

} // namespace

int main() {
#if defined(_WIN32)
    std::clog << "test_web_tools: skip on WIN32\n";
    return 0;
#else
    using namespace agent_framework;

    const std::string data_dir = AGENT_TEST_WEB_DATA_DIR;
    const std::string ddg_html = read_all(data_dir + "/ddg_sample.html");
    assert(!ddg_html.empty());
    const auto hits = parse_duckduckgo_html_results(ddg_html, 10);
    assert(hits.size() >= 1);
    assert(hits[0].title.find("Example") != std::string::npos);
    assert(hits[0].url.find("example.com") != std::string::npos);
    if (web_test_show_query_results()) {
        std::clog << "[test_web_tools] parse_duckduckgo_html_results: " << hits.size() << " hit(s)\n";
        for (std::size_t i = 0; i < hits.size(); ++i) {
            std::clog << "  [" << i << "] " << hits[i].title << "\n      " << hits[i].url << "\n";
        }
    }

    const json searx_fixture = {
        {"results", json::array({{{"title", "GNSS result"},
                                   {"url", "https://example.com/gnss"},
                                   {"content", "SearXNG snippet"}},
                                  {{"title", "invalid"}, {"url", "file:///etc/passwd"}}})}};
    const auto searx_hits = parse_searxng_json_results(searx_fixture, 10);
    assert(searx_hits.size() == 1);
    assert(searx_hits[0].snippet == "SearXNG snippet");

    {
        const json invalid = agent_framework::web_search(
            json{{"query", "GNSS"}, {"provider", "not-a-provider"}});
        assert(invalid.contains("error"));
        assert(invalid["error"]["code"] == "search_provider_config_error");
    }

    (void)::setenv("AGENT_WEB_ENABLE", "1", 1);
    (void)::setenv("AGENT_WEB_TEST_ALLOW_LOOPBACK", "1", 1);
    (void)::setenv("AGENT_WEB_ALLOW_HTTP", "1", 1);
    (void)::setenv("AGENT_TOOL_ALLOWLIST", "", 1);

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "agent_web_tools_test";
    fs::remove_all(root);
    assert(fs::create_directories(root / "sub"));
    (void)::setenv("AGENT_FS_ROOT", root.string().c_str(), 1);

    httplib::Server srv;
    std::string zip_body = read_all(data_dir + "/minimal_stored.zip");
    assert(!zip_body.empty());
    srv.Get("/minimal.zip", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(zip_body, "application/zip");
    });
    int port = 0;
    srv.Get("/search", [&](const httplib::Request& req, httplib::Response& res) {
        assert(req.has_param("q"));
        assert(req.get_param_value("format") == "json");
        json results = json::array({{{"title", "Local result"},
                                     {"url", "http://127.0.0.1:" + std::to_string(port) + "/article"},
                                     {"content", "Search snippet"}}});
        if (req.get_param_value("q") == "mixed") {
            results.push_back({{"title", "Malformed result"},
                               {"url", "http://127.0.0.1:" + std::to_string(port) + "/oversized-tag"},
                               {"content", "must fail in isolation"}});
        }
        json body = {{"results", std::move(results)}};
        res.set_content(body.dump(), "application/json");
    });
    srv.Get("/article", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content("<html><head><style>.hidden{display:none}</style><script>bad()</script></head>"
                        "<body><main>GNSS-R &amp; extracted article body</main></body></html>",
                        "text/html; charset=utf-8");
    });
    srv.Get("/oversized-tag", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content("<div " + std::string(20000, 'x') + ">must not be returned</div>",
                        "text/html; charset=utf-8");
    });
    srv.Get("/adversarial", [&](const httplib::Request&, httplib::Response& res) {
        std::string body = "<html><script data-x='" + std::string(60000, 'a') + "'>";
        body += std::string(60000, '<');
        body += "</script><body>safe tail</body></html>";
        res.set_content(body, "text/html; charset=utf-8");
    });
    port = srv.bind_to_any_port("127.0.0.1");
    assert(port > 0);
    std::thread th([&]() { srv.listen_after_bind(); });
    for (int i = 0; i < 50; ++i) {
        if (srv.is_running()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(srv.is_running());

    ToolBus bus;
    register_builtin_web_tools_if_configured(bus);
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    srv.stop();
    th.join();
    std::clog << "test_web_tools: skip network tools (no OpenSSL)\n";
    return 0;
#else
    assert(bus.get_tool_info("web_search").has_value());
    assert(bus.get_tool_info("web_fetch").has_value());

    {
        const std::string endpoint = "http://127.0.0.1:" + std::to_string(port);
        (void)::setenv("AGENT_WEB_SEARXNG_URL", endpoint.c_str(), 1);
        (void)::setenv("AGENT_WEB_SEARCH_PROVIDER", "searxng", 1);
        (void)::setenv("AGENT_WEB_SEARCH_FALLBACK", "none", 1);
        json r = bus.call_tool("web_search",
                               json{{"query", "GNSS-R"}, {"max_results", 2}})
                     .get();
        if (r.contains("error")) {
            std::cerr << "searxng search err: " << r.dump() << "\n";
        }
        assert(!r.contains("error"));
        assert(r["provider"] == "searxng");
        assert(r["results"].size() == 1);
        assert(r["results"][0]["content_status"] == "fetched");
        const auto content = r["results"][0]["content"].get<std::string>();
        assert(content.find("GNSS-R") != std::string::npos);
        assert(content.find("extracted article body") != std::string::npos);
        assert(content.find("bad()") == std::string::npos);
        assert(content.find("display:none") == std::string::npos);
        assert(r["content_enrichment"]["attempted"] == 1);
        assert(r["content_enrichment"]["succeeded"] == 1);
        assert(r["content_enrichment"]["failed"] == 0);
        clog_tool_json_for_test("web_search (SearXNG + content)", r);
    }

    {
        json r = bus.call_tool("web_search",
                               json{{"query", "mixed"}, {"max_results", 3},
                                    {"content_max_bytes", 65536}}).get();
        assert(!r.contains("error"));
        assert(r["results"].size() == 2);
        assert(r["results"][0]["content_status"] == "fetched");
        assert(r["results"][1]["content_status"] == "failed");
        assert(r["results"][1]["content_error"]["code"] == "html_tag_too_large");
        assert(r["content_enrichment"]["succeeded"] == 1);
        assert(r["content_enrichment"]["failed"] == 1);
        assert(r["content_enrichment"]["failure_isolation"] == "per_url");
    }

    {
        json r = bus.call_tool("web_fetch",
                               json{{"url", "http://127.0.0.1:" + std::to_string(port) + "/adversarial"},
                                    {"max_bytes", 131072}, {"extract_mode", "main_text"}}).get();
        assert(r.contains("error"));
        assert(r["error"]["code"] == "html_tag_too_large");
    }

    {
        std::string adversarial = "<a class=\"result__a\" href=\"https://example.com/a\">";
        adversarial += std::string(200000, '<');
        adversarial += "title</a><a class=\"result__snippet\">snippet</a>";
        const auto parsed = parse_duckduckgo_html_results(adversarial, 10);
        assert(parsed.size() == 1);
        assert(parsed[0].url == "https://example.com/a");
    }

    {
        json r = bus.call_tool("web_fetch", json{{"url", "http://10.0.0.1:65530/foo"}, {"max_bytes", 1024}})
                     .get();
        assert(r.contains("error"));
        assert(r["error"]["code"] == "url_disallowed");
    }
    {
        json r = bus
                     .call_tool("web_fetch",
                                json{{"url", "http://127.0.0.1:" + std::to_string(port) + "/minimal.zip"},
                                     {"max_bytes", static_cast<int>(zip_body.size() + 1024)}})
                     .get();
        if (r.contains("error")) {
            std::cerr << "web_fetch err: " << r.dump() << "\n";
        }
        assert(!r.contains("error"));
        assert(r.contains("binary_preview_hex"));
        clog_tool_json_for_test("web_fetch (loopback zip)", r);
    }

    const std::string rss_xml = R"(
<rss version="2.0"><channel>
<item><title>GNSS News</title><link>https://example.com/a</link><description>GPS test</description>
<pubDate>Mon, 01 Jan 2024 00:00:00 GMT</pubDate>
</item>
</channel></rss>)";
    srv.Get("/feed.xml", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(rss_xml, "application/rss+xml");
    });
    {
        json r =
            bus
                .call_tool("web_rss_feed",
                           json{{"feed_url", "http://127.0.0.1:" + std::to_string(port) + "/feed.xml"},
                                {"max_entries", 5},
                                {"max_age_hours", 0},
                                {"keywords", json::array({"gnss"})}})
                .get();
        assert(!r.contains("error"));
        assert(r.contains("entries"));
        assert(r["entries"].is_array());
        assert(r["entries"].size() >= 1);
        clog_tool_json_for_test("web_rss_feed (loopback)", r);
    }

    register_builtin_fs_tools_if_configured(bus);
    {
        json r =
            bus
                .call_tool("web_fetch_archive",
                           json{{"url", "http://127.0.0.1:" + std::to_string(port) + "/minimal.zip"}})
                .get();
        if (r.contains("error")) {
            std::cerr << "archive: " << r.dump() << "\n";
        }
        assert(!r.contains("error"));
        assert(r.contains("files"));
        assert(r["files"].size() >= 1);
        clog_tool_json_for_test("web_fetch_archive (minimal.zip)", r);
    }
    {
        std::string bad_zip = read_all(data_dir + "/zip_slip.zip");
        assert(!bad_zip.empty());
        srv.Get("/bad.zip", [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(bad_zip, "application/zip");
        });
        json r = bus
                     .call_tool("web_fetch_archive",
                                json{{"url", "http://127.0.0.1:" + std::to_string(port) + "/bad.zip"}})
                     .get();
        assert(r.contains("error"));
        assert(r["error"]["code"] == "archive_path_escape");
    }

    srv.stop();
    th.join();

    run_live_ddg_search_samples(bus);

    std::clog << "test_web_tools: ok\n";
    return 0;
#endif // CPPHTTPLIB_OPENSSL_SUPPORT
#endif // !_WIN32
}
