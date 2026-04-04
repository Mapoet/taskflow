/**
 * @file test_news_sources.cpp
 * @brief news_sources 目录 v1 解析与 web_configured_source 离线 mock 测试
 */

#include <agent/news_sources_catalog.hpp>
#include <agent/news_sources_tool.hpp>
#include <agent/toolbus.hpp>
#include <agent/web_tools.hpp>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#if __has_include(<httplib/httplib.hpp>)
#include <httplib/httplib.hpp>
#elif __has_include(<httplib.hpp>)
#include <httplib.hpp>
#endif

#ifndef AGENT_TEST_NEWS_JSON
#define AGENT_TEST_NEWS_JSON "tests/data/news_sources_test_catalog.json"
#endif

int main() {
#if defined(_WIN32)
    std::clog << "test_news_sources: skip on WIN32\n";
    return 0;
#else
    using namespace agent_framework;

    std::string err;
    assert(!parse_news_sources_catalog(json{{"version", 2}}, err));

    json minimal = json::parse(R"({"version":1,"rss":{"x":{"url":"https://example.com/f"}}})");
    auto cat = parse_news_sources_catalog(minimal, err);
    assert(cat != nullptr);
    assert(cat->find_rss("x").has_value());

    NewsApiEntry ae;
    ae.url = "http://h.test/p";
    ae.params = json{{"z", 1}, {"a", 2}};
    std::string be;
    const std::string built = NewsSourcesCatalog::build_api_get_url(ae, be);
    assert(be.empty());
    assert(built == "http://h.test/p?a=2&z=1");

    json bad_en =
        json::parse(R"({"version":1,"rss":{"x":{"url":"http://u","enabled":"notbool"}}})");
    assert(!parse_news_sources_catalog(bad_en, err));

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    std::clog << "test_news_sources: skip network (no OpenSSL)\n";
    return 0;
#else
    (void)::setenv("AGENT_WEB_ENABLE", "1", 1);
    (void)::setenv("AGENT_WEB_TEST_ALLOW_LOOPBACK", "1", 1);
    (void)::setenv("AGENT_WEB_ALLOW_HTTP", "1", 1);
    (void)::setenv("AGENT_TOOL_ALLOWLIST", "", 1);
    (void)::setenv("AGENT_NEWS_SOURCES_JSON", AGENT_TEST_NEWS_JSON, 1);

    const int port = 18102;
    httplib::Server srv;
    srv.Get("/rss", [&](const httplib::Request&, httplib::Response& res) {
        const char* xml = "<rss version=\"2.0\"><channel><item><title>T</title>"
                          "<link>https://ex.test</link><description>D</description>"
                          "</item></channel></rss>";
        res.set_content(xml, "application/rss+xml");
    });
    srv.Get("/api", [&](const httplib::Request& req, httplib::Response& res) {
        assert(req.has_header("X-Test-Header"));
        assert(req.get_header_value("X-Test-Header") == "hello");
        assert(req.has_param("a"));
        assert(req.has_param("z"));
        assert(req.get_param_value("a") == "2");
        assert(req.get_param_value("z") == "1");
        res.set_content("{\"ok\":true}", "application/json");
    });
    srv.Get("/page", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content("<html><body>hi</body></html>", "text/html");
    });

    std::thread th([&]() { srv.listen("127.0.0.1", port); });
    for (int i = 0; i < 50; ++i) {
        if (srv.is_running()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(srv.is_running());

    ToolBus bus;
    register_builtin_web_tools_if_configured(bus);
    register_web_configured_source_if_configured(bus);
    assert(bus.get_tool_info("web_configured_source").has_value());

    {
        json r = bus.call_tool("web_configured_source",
                               json{{"kind", "rss"},
                                    {"source_id", "local_rss"},
                                    {"max_age_hours", 0},
                                    {"skip_keyword_filter", true}})
                   .get();
        assert(!r.contains("error"));
        assert(r.contains("entries"));
        assert(r["catalog_kind"] == "rss");
    }
    {
        json r =
            bus.call_tool("web_configured_source", json{{"kind", "api"}, {"source_id", "local_api"}})
                .get();
        assert(!r.contains("error"));
        assert(r.contains("json"));
        assert(r["json"]["ok"] == true);
        assert(r["catalog_kind"] == "api");
    }
    {
        json r = bus
                     .call_tool("web_configured_source",
                                json{{"kind", "scrape"}, {"source_id", "local_scrape"}})
                     .get();
        assert(!r.contains("error"));
        assert(r.contains("html") || r.contains("text"));
        assert(r["catalog_base_url"] == "http://127.0.0.1:18102");
    }
    {
        json r = bus.call_tool("web_configured_source", json{{"kind", "rss"}, {"source_id", "nope"}})
                     .get();
        assert(r.contains("error"));
        assert(r["error"]["code"] == "unknown_source");
    }

    json bad_ua =
        json{{"url", "http://127.0.0.1:" + std::to_string(port) + "/api?a=1"},
             {"headers", json{{"User-Agent", "evil"}}}};
    json fr = do_web_fetch(bad_ua);
    assert(fr.contains("error"));
    assert(fr["error"]["code"] == "invalid_arguments");

    srv.stop();
    th.join();

    std::clog << "test_news_sources: ok\n";
    return 0;
#endif
#endif
}
