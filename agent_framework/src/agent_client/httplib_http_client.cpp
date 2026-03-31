/**
 * @file httplib_http_client.cpp
 * @brief cpp-httplib 实现的 HTTPClient
 */
#include <agent/httplib_http_client.hpp>

#include <cctype>
#include <sstream>
#include <stdexcept>

#if __has_include(<httplib/httplib.hpp>)
#include <httplib/httplib.hpp>
#elif __has_include(<httplib.hpp>)
#include <httplib.hpp>
#elif __has_include(<httplib.h>)
#include <httplib.h>
#else
#error "httplib not found. Ensure 3rd-party/httplib is available."
#endif

namespace agent_framework {

namespace {

struct ParsedHttpUrl {
    std::string scheme_host_port; // e.g. http://localhost:8080
    std::string path_and_query;   // e.g. /tasks/send?x=1
};

void trim_in_place(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
}

/**
 * @brief 将绝对 http(s) URL 拆成 httplib::Client 构造串与 path（含 query）
 */
ParsedHttpUrl parse_absolute_url(const std::string& url_raw) {
    std::string url = url_raw;
    trim_in_place(url);
    if (url.empty()) {
        throw std::runtime_error("HttplibClient: empty URL");
    }

    const std::string http = "http://";
    const std::string https = "https://";
    std::size_t after_scheme = 0;
    if (url.rfind(http, 0) == 0) {
        after_scheme = http.size();
    } else if (url.rfind(https, 0) == 0) {
        after_scheme = https.size();
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
        throw std::runtime_error(
            "HttplibClient: https requires OpenSSL (define CPPHTTPLIB_OPENSSL_SUPPORT and link OpenSSL)");
#endif
    } else {
        throw std::runtime_error(
            "HttplibClient: URL must start with http:// or https://, got: " + url.substr(0, 32));
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
    out.scheme_host_port = authority;
    out.path_and_query = path_query;
    return out;
}

void apply_default_timeouts(httplib::Client& cli) {
    cli.set_connection_timeout(CPPHTTPLIB_CONNECTION_TIMEOUT_SECOND,
                               CPPHTTPLIB_CONNECTION_TIMEOUT_USECOND);
    cli.set_read_timeout(CPPHTTPLIB_READ_TIMEOUT_SECOND, CPPHTTPLIB_READ_TIMEOUT_USECOND);
    cli.set_write_timeout(CPPHTTPLIB_WRITE_TIMEOUT_SECOND, CPPHTTPLIB_WRITE_TIMEOUT_USECOND);
}

std::string truncate_body(const std::string& body, std::size_t max_len) {
    if (body.size() <= max_len) {
        return body;
    }
    return body.substr(0, max_len) + "...";
}

httplib::Headers to_httplib_headers(const std::map<std::string, std::string>& headers) {
    httplib::Headers h;
    for (const auto& kv : headers) {
        h.emplace(kv.first, kv.second);
    }
    return h;
}

json execute_and_parse_json(const httplib::Result& result, const char* method_label) {
    if (!result) {
        std::ostringstream oss;
        oss << "HttplibClient: " << method_label << " failed, error code: "
            << static_cast<int>(result.error());
        throw std::runtime_error(oss.str());
    }

    const auto& res = result.value();
    const int status = res.status;

    if (status < 200 || status >= 300) {
        std::ostringstream oss;
        oss << "HttplibClient: HTTP " << status << " on " << method_label << ": "
            << truncate_body(res.body, 512);
        throw std::runtime_error(oss.str());
    }

    if (res.body.empty()) {
        return json::object();
    }

    try {
        return json::parse(res.body);
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("HttplibClient: invalid JSON in response: ") + e.what() +
                                 " body=" + truncate_body(res.body, 256));
    }
}

} // namespace

json HttplibClient::post(const std::string& url, const json& body,
                         const std::map<std::string, std::string>& headers) {
    ParsedHttpUrl parsed = parse_absolute_url(url);
    httplib::Client cli(parsed.scheme_host_port.c_str());
    if (!cli.is_valid()) {
        throw std::runtime_error("HttplibClient: invalid client for URL: " + parsed.scheme_host_port);
    }
    apply_default_timeouts(cli);

    httplib::Headers h = to_httplib_headers(headers);
    const std::string payload = body.dump();
    auto result = cli.Post(parsed.path_and_query.c_str(), h, payload, "application/json");
    return execute_and_parse_json(result, "POST");
}

json HttplibClient::get(const std::string& url,
                          const std::map<std::string, std::string>& headers) {
    ParsedHttpUrl parsed = parse_absolute_url(url);
    httplib::Client cli(parsed.scheme_host_port.c_str());
    if (!cli.is_valid()) {
        throw std::runtime_error("HttplibClient: invalid client for URL: " + parsed.scheme_host_port);
    }
    apply_default_timeouts(cli);

    httplib::Headers h = to_httplib_headers(headers);
    auto result = cli.Get(parsed.path_and_query.c_str(), h);
    return execute_and_parse_json(result, "GET");
}

} // namespace agent_framework
