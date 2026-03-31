/**
 * @file http_transport.cpp
 * @brief MCP HTTP 传输（单 POST JSON-RPC）
 */

#include "agent/mcp_client.hpp"
#include "agent/internal/http_sse.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <utility>

#if __has_include(<httplib/httplib.hpp>)
#include <httplib/httplib.hpp>
#elif __has_include(<httplib.hpp>)
#include <httplib.hpp>
#elif __has_include(<httplib.h>)
#include <httplib.h>
#else
#error "httplib not found for HttpMCPTransport"
#endif

namespace agent_framework {

namespace {

struct ParsedHttpUrl {
    std::string scheme_host_port;
    std::string path_and_query;
};

void trim_in_place(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
}

ParsedHttpUrl parse_absolute_url(const std::string& url_raw) {
    std::string url = url_raw;
    trim_in_place(url);
    if (url.empty()) {
        throw std::runtime_error("HttpMCPTransport: empty URL");
    }
    const std::string http = "http://";
    const std::string https = "https://";
    std::size_t after_scheme = 0;
    if (url.rfind(http, 0) == 0) {
        after_scheme = http.size();
    } else if (url.rfind(https, 0) == 0) {
        after_scheme = https.size();
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
        throw std::runtime_error("HttpMCPTransport: https requires OpenSSL support in build");
#endif
    } else {
        throw std::runtime_error("HttpMCPTransport: URL must start with http:// or https://");
    }
    const std::size_t path_start = url.find('/', after_scheme);
    std::string authority;
    std::string path_query;
    if (path_start == std::string::npos) {
        authority = url;
        path_query = "/";
    } else {
        authority = url.substr(0, path_start);
        path_query = url.substr(path_start);
        if (path_query.empty()) {
            path_query = "/";
        }
    }
    ParsedHttpUrl out;
    out.scheme_host_port = std::move(authority);
    out.path_and_query = std::move(path_query);
    return out;
}

int mcp_timeout_sec() {
    const char* e = std::getenv("AGENT_MCP_REQUEST_TIMEOUT_MS");
    if (e == nullptr || e[0] == '\0') {
        return 60;
    }
    int ms = std::atoi(e);
    if (ms <= 0) {
        return 60;
    }
    return (ms + 999) / 1000;
}

httplib::Headers to_httplib_headers(const std::map<std::string, std::string>& headers) {
    httplib::Headers h;
    for (const auto& kv : headers) {
        h.emplace(kv.first, kv.second);
    }
    return h;
}

} // namespace

HttpMCPTransport::HttpMCPTransport(std::string post_url, std::map<std::string, std::string> extra_headers)
    : post_url_(std::move(post_url)), headers_(std::move(extra_headers)) {}

bool HttpMCPTransport::connect(const std::string& /*endpoint*/) {
    (void)parse_absolute_url(post_url_);
    connected_ = true;
    return true;
}

void HttpMCPTransport::disconnect() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    connected_ = false;
}

json HttpMCPTransport::post_json(const json& body) {
    ParsedHttpUrl u = parse_absolute_url(post_url_);
    httplib::Client cli(u.scheme_host_port.c_str());
    int sec = mcp_timeout_sec();
    cli.set_connection_timeout(sec, 0);
    cli.set_read_timeout(sec, 0);
    cli.set_write_timeout(sec, 0);

    std::string payload = body.dump();
    httplib::Headers h = to_httplib_headers(headers_);
    auto res = cli.Post(u.path_and_query.c_str(), h, payload, "application/json");
    if (!res) {
        throw std::runtime_error("HttpMCPTransport: HTTP request failed (network)");
    }
    if (res->status < 200 || res->status >= 300) {
        throw std::runtime_error("HttpMCPTransport: HTTP status " + std::to_string(res->status) + " body: " +
                                 res->body.substr(0, 512));
    }
    if (res->has_header("Mcp-Session-Id")) {
        headers_["Mcp-Session-Id"] = res->get_header_value("Mcp-Session-Id");
    }
    if (res->body.empty()) {
        return json::object();
    }
    std::string content_type;
    if (res->has_header("Content-Type")) {
        content_type = res->get_header_value("Content-Type");
    }
    if (internal::icontains(content_type, "text/event-stream")) {
        const std::string json_text = internal::parse_sse_body_to_json_text(res->body);
        return json::parse(json_text);
    }
    return json::parse(res->body);
}

json HttpMCPTransport::transceive(const json& jsonrpc_request) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!connected_) {
        throw std::runtime_error("HttpMCPTransport: not connected");
    }
    return post_json(jsonrpc_request);
}

void HttpMCPTransport::send_notification(const json& jsonrpc_notification) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!connected_) {
        throw std::runtime_error("HttpMCPTransport: not connected");
    }
    (void)post_json(jsonrpc_notification);
}

bool HttpMCPTransport::is_connected() const {
    return connected_;
}

MCPTransport HttpMCPTransport::get_transport_type() const {
    return MCPTransport::HTTP;
}

} // namespace agent_framework
