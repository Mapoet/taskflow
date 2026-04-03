/**
 * @file test_web_tools.cpp
 * @brief web_* 内建工具：DDG 解析 fixture、mock HTTP、SSRF、RSS、ZIP 解压（无公网）
 */

#include <agent/fs_tools.hpp>
#include <agent/toolbus.hpp>
#include <agent/web_search_ddg.hpp>
#include <agent/web_tools.hpp>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
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

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
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

    (void)::setenv("AGENT_WEB_ENABLE", "1", 1);
    (void)::setenv("AGENT_WEB_TEST_ALLOW_LOOPBACK", "1", 1);
    (void)::setenv("AGENT_WEB_ALLOW_HTTP", "1", 1);
    (void)::setenv("AGENT_TOOL_ALLOWLIST", "", 1);

    httplib::Server srv;
    std::string zip_body = read_all(data_dir + "/minimal_stored.zip");
    assert(!zip_body.empty());
    srv.Get("/minimal.zip", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(zip_body, "application/zip");
    });
    const int port = 8099;
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
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    srv.stop();
    th.join();
    std::clog << "test_web_tools: skip network tools (no OpenSSL)\n";
    return 0;
#else
    assert(bus.get_tool_info("web_search").has_value());
    assert(bus.get_tool_info("web_fetch").has_value());

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
    }

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "agent_web_tools_test";
    fs::remove_all(root);
    assert(fs::create_directories(root / "sub"));
    (void)::setenv("AGENT_FS_ROOT", root.string().c_str(), 1);
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

    std::clog << "test_web_tools: ok\n";
    return 0;
#endif // CPPHTTPLIB_OPENSSL_SUPPORT
#endif // !_WIN32
}
