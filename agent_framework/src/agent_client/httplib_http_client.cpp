/**
 * @file httplib_http_client.cpp
 * @brief cpp-httplib 实现的 HTTPClient
 */
#include <agent/httplib_http_client.hpp>
#include <agent/types.hpp>

#include <cctype>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

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
    std::string scheme_host_port; // e.g. http://localhost:8080（调试用）
    std::string path_and_query;   // e.g. /tasks/send?x=1
    std::string host;             // 不含 scheme，如 127.0.0.1 或 [::1]
    int port = 80;
    bool is_ssl = false;
};

/**
 * @brief 从 authority 中 host[:port] 段解析 host 与端口（支持 IPv6 [addr]:port）
 */
void parse_host_and_port(std::string_view hp, bool is_ssl, std::string& host_out, int& port_out) {
    port_out = is_ssl ? 443 : 80;
    if (hp.empty()) {
        throw std::runtime_error("HttplibClient: empty host in URL");
    }
    if (hp.front() == '[') {
        const std::size_t closing = hp.find(']');
        if (closing == std::string_view::npos) {
            throw std::runtime_error("HttplibClient: malformed IPv6 host in URL");
        }
        host_out = std::string(hp.substr(1, closing - 1));
        if (closing + 1 < hp.size()) {
            if (hp[closing + 1] != ':') {
                throw std::runtime_error("HttplibClient: malformed host:port after IPv6");
            }
            port_out = std::stoi(std::string(hp.substr(closing + 2)));
        }
        return;
    }
    const std::size_t colon = hp.rfind(':');
    if (colon != std::string_view::npos && colon > 0) {
        bool port_digits = true;
        for (std::size_t i = colon + 1; i < hp.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(hp[i]))) {
                port_digits = false;
                break;
            }
        }
        if (port_digits && colon + 1 < hp.size()) {
            host_out = std::string(hp.substr(0, colon));
            port_out = std::stoi(std::string(hp.substr(colon + 1)));
            return;
        }
    }
    host_out = std::string(hp);
}

void trim_in_place(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
}

/**
 * @brief 将绝对 http(s) URL 拆成 host、port、path（含 query），供 Client(host,port) 使用
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
    bool is_ssl = false;
    if (url.rfind(http, 0) == 0) {
        after_scheme = http.size();
    } else if (url.rfind(https, 0) == 0) {
        after_scheme = https.size();
        is_ssl = true;
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
    out.is_ssl = is_ssl;
    constexpr std::string_view http_sv("http://");
    constexpr std::string_view https_sv("https://");
    const std::string_view auth_view(authority);
    if (is_ssl) {
        if (auth_view.size() < https_sv.size() || auth_view.compare(0, https_sv.size(), https_sv) != 0) {
            throw std::runtime_error("HttplibClient: internal URL parse error");
        }
        parse_host_and_port(auth_view.substr(https_sv.size()), is_ssl, out.host, out.port);
    } else {
        if (auth_view.size() < http_sv.size() || auth_view.compare(0, http_sv.size(), http_sv) != 0) {
            throw std::runtime_error("HttplibClient: internal URL parse error");
        }
        parse_host_and_port(auth_view.substr(http_sv.size()), is_ssl, out.host, out.port);
    }
    return out;
}

template <typename ClientLike>
void apply_client_timeouts(ClientLike& cli, int sec) {
    if (sec > 0) {
        cli.set_connection_timeout(sec, 0);
        cli.set_read_timeout(sec, 0);
        cli.set_write_timeout(sec, 0);
    } else {
        cli.set_connection_timeout(CPPHTTPLIB_CONNECTION_TIMEOUT_SECOND,
                                   CPPHTTPLIB_CONNECTION_TIMEOUT_USECOND);
        cli.set_read_timeout(CPPHTTPLIB_READ_TIMEOUT_SECOND, CPPHTTPLIB_READ_TIMEOUT_USECOND);
        cli.set_write_timeout(CPPHTTPLIB_WRITE_TIMEOUT_SECOND, CPPHTTPLIB_WRITE_TIMEOUT_USECOND);
    }
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

namespace {

std::optional<int> parse_retry_after_sec(const httplib::Headers& hdrs) {
    for (const auto& h : hdrs) {
        std::string key = h.first;
        for (auto& c : key) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (key == "retry-after") {
            return std::optional<int>(std::atoi(h.second.c_str()));
        }
    }
    return std::nullopt;
}

} // namespace

json HttplibClient::post_llm(const std::string& url, const json& body,
                             const std::map<std::string, std::string>& headers,
                             const std::string& provider) {
    ParsedHttpUrl parsed = parse_absolute_url(url);
    httplib::Headers h = to_httplib_headers(headers);
    const std::string payload = body.dump();

    httplib::Result result(nullptr, httplib::Error::Unknown);
    if (parsed.is_ssl) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        httplib::SSLClient ssl_cli(parsed.host, parsed.port);
        if (!ssl_cli.is_valid()) {
            throw llm_http_error(0, provider, "HttplibClient: invalid SSL client for " + parsed.scheme_host_port,
                                 std::nullopt);
        }
        apply_client_timeouts(ssl_cli, timeout_sec_);
        result = ssl_cli.Post(parsed.path_and_query.c_str(), h, payload, "application/json");
#else
        throw std::runtime_error("HttplibClient: https requires OpenSSL");
#endif
    } else {
        httplib::Client cli(parsed.host, parsed.port);
        apply_client_timeouts(cli, timeout_sec_);
        result = cli.Post(parsed.path_and_query.c_str(), h, payload, "application/json");
    }
    if (!result) {
        std::ostringstream oss;
        oss << "POST transport error " << static_cast<int>(result.error());
        throw llm_http_error(0, provider, oss.str(), std::nullopt);
    }
    const auto& res = result.value();
    const int status = res.status;
    if (status < 200 || status >= 300) {
        throw llm_http_error(status, provider, truncate_body(res.body, 512), parse_retry_after_sec(res.headers));
    }
    if (res.body.empty()) {
        return json::object();
    }
    try {
        return json::parse(res.body);
    } catch (const json::exception& e) {
        throw llm_http_error(status, provider,
                             std::string("invalid JSON: ") + e.what() + " " + truncate_body(res.body, 256),
                             std::nullopt);
    }
}

json HttplibClient::post(const std::string& url, const json& body,
                         const std::map<std::string, std::string>& headers) {
    ParsedHttpUrl parsed = parse_absolute_url(url);
    httplib::Headers h = to_httplib_headers(headers);
    const std::string payload = body.dump();

    httplib::Result result(nullptr, httplib::Error::Unknown);
    if (parsed.is_ssl) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        httplib::SSLClient ssl_cli(parsed.host, parsed.port);
        if (!ssl_cli.is_valid()) {
            throw std::runtime_error("HttplibClient: invalid SSL client for URL: " + parsed.scheme_host_port);
        }
        apply_client_timeouts(ssl_cli, timeout_sec_);
        result = ssl_cli.Post(parsed.path_and_query.c_str(), h, payload, "application/json");
#else
        throw std::runtime_error("HttplibClient: https requires OpenSSL");
#endif
    } else {
        httplib::Client cli(parsed.host, parsed.port);
        apply_client_timeouts(cli, timeout_sec_);
        result = cli.Post(parsed.path_and_query.c_str(), h, payload, "application/json");
    }
    return execute_and_parse_json(result, "POST");
}

json HttplibClient::get(const std::string& url,
                          const std::map<std::string, std::string>& headers) {
    ParsedHttpUrl parsed = parse_absolute_url(url);
    httplib::Headers h = to_httplib_headers(headers);

    httplib::Result result(nullptr, httplib::Error::Unknown);
    if (parsed.is_ssl) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        httplib::SSLClient ssl_cli(parsed.host, parsed.port);
        if (!ssl_cli.is_valid()) {
            throw std::runtime_error("HttplibClient: invalid SSL client for URL: " + parsed.scheme_host_port);
        }
        apply_client_timeouts(ssl_cli, timeout_sec_);
        result = ssl_cli.Get(parsed.path_and_query.c_str(), h);
#else
        throw std::runtime_error("HttplibClient: https requires OpenSSL");
#endif
    } else {
        httplib::Client cli(parsed.host, parsed.port);
        apply_client_timeouts(cli, timeout_sec_);
        result = cli.Get(parsed.path_and_query.c_str(), h);
    }
    return execute_and_parse_json(result, "GET");
}

void HttplibClient::get_sse(const std::string& url,
                            const std::map<std::string, std::string>& headers,
                            const std::function<void(std::string_view chunk)>& on_chunk,
                            int timeout_sec,
                            const std::atomic<bool>* cancel_flag) {
    ParsedHttpUrl parsed = parse_absolute_url(url);
    const int eff = timeout_sec > 0 ? timeout_sec : timeout_sec_;

    httplib::Headers h = to_httplib_headers(headers);
    if (!httplib::detail::has_header(h, "Accept")) {
        h.emplace("Accept", "text/event-stream");
    }

    bool headers_ok = true;
    int response_status = -1;

    auto response_handler = [&](const httplib::Response& res) {
        response_status = res.status;
        headers_ok = (res.status >= 200 && res.status < 300);
        return true;
    };
    auto content_receiver = [&](const char* data, std::size_t data_length) {
        if (cancel_flag && cancel_flag->load(std::memory_order_acquire)) {
            return false;
        }
        if (!headers_ok) {
            return true;
        }
        on_chunk(std::string_view(data, data_length));
        return true;
    };

    httplib::Result result(nullptr, httplib::Error::Unknown);
    if (parsed.is_ssl) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        httplib::SSLClient ssl_cli(parsed.host, parsed.port);
        if (!ssl_cli.is_valid()) {
            throw std::runtime_error("HttplibClient: invalid SSL client for URL: " + parsed.scheme_host_port);
        }
        apply_client_timeouts(ssl_cli, eff);
        result = ssl_cli.Get(parsed.path_and_query.c_str(), h, response_handler, content_receiver);
#else
        throw std::runtime_error("HttplibClient: https requires OpenSSL");
#endif
    } else {
        httplib::Client cli(parsed.host, parsed.port);
        apply_client_timeouts(cli, eff);
        result = cli.Get(parsed.path_and_query.c_str(), h, response_handler, content_receiver);
    }

    if (!result) {
        const auto err = result.error();
        if (err == httplib::Error::Read || err == httplib::Error::Canceled) {
            return;
        }
        std::ostringstream oss;
        oss << "HttplibClient: get_sse transport error " << static_cast<int>(err);
        throw std::runtime_error(oss.str());
    }

    if (response_status >= 200 && response_status < 300) {
        return;
    }
    std::ostringstream oss;
    oss << "HttplibClient: get_sse HTTP " << response_status;
    throw std::runtime_error(oss.str());
}

void HttplibClient::post_sse(const std::string& url, const json& body,
                             const std::map<std::string, std::string>& headers,
                             const std::function<void(const std::string& event_name, const json& data)>&
                                 on_event,
                             int timeout_sec) {
    ParsedHttpUrl parsed = parse_absolute_url(url);
    const int eff = timeout_sec > 0 ? timeout_sec : timeout_sec_;

    httplib::Request req;
    req.method = "POST";
    req.path = parsed.path_and_query;
    req.headers = to_httplib_headers(headers);
    req.headers.emplace("Content-Type", "application/json");
    if (!req.has_header("Accept")) {
        req.headers.emplace("Accept", "text/event-stream");
    }
    req.body = body.dump();

    std::string sse_line_buffer;
    std::string current_event;
    std::string error_body_accum;
    int response_status = -1;
    bool headers_ok = true;

    req.response_handler_ = [&](const httplib::Response& res) {
        response_status = res.status;
        if (res.status < 200 || res.status >= 300) {
            headers_ok = false;
        }
        return true;
    };

    req.content_receiver_ =
        [&](const char* data, std::size_t data_length, std::uint64_t /*off*/, std::uint64_t /*total*/) {
            if (!headers_ok) {
                error_body_accum.append(data, data_length);
                return true;
            }
            sse_line_buffer.append(data, data_length);
            for (;;) {
                const auto nlp = sse_line_buffer.find('\n');
                if (nlp == std::string::npos) {
                    break;
                }
                std::string line = sse_line_buffer.substr(0, nlp);
                sse_line_buffer.erase(0, nlp + 1);
                while (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                trim_in_place(line);
                if (line.empty()) {
                    continue;
                }
                constexpr const char* k_data = "data:";
                constexpr const char* k_event = "event:";
                if (line.rfind(k_event, 0) == 0) {
                    current_event = line.substr(std::strlen(k_event));
                    trim_in_place(current_event);
                    continue;
                }
                if (line.rfind(k_data, 0) == 0) {
                    std::string payload = line.substr(std::strlen(k_data));
                    trim_in_place(payload);
                    if (payload == "[DONE]") {
                        current_event.clear();
                        continue;
                    }
                    try {
                        json j = json::parse(payload);
                        on_event(current_event, std::move(j));
                    } catch (const json::exception&) {
                        // 跳过无法解析的行（注释/心跳）
                    }
                    current_event.clear();
                }
            }
            return true;
        };

    httplib::Response res;
    httplib::Error err = httplib::Error::Success;
    bool ok = false;
    if (parsed.is_ssl) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        httplib::SSLClient ssl_cli(parsed.host, parsed.port);
        if (!ssl_cli.is_valid()) {
            throw llm_http_error(0, "httplib", "HttplibClient: invalid SSL client for " + parsed.scheme_host_port,
                                 std::nullopt);
        }
        apply_client_timeouts(ssl_cli, eff);
        ok = ssl_cli.send(req, res, err);
#else
        throw std::runtime_error("HttplibClient: https requires OpenSSL");
#endif
    } else {
        httplib::Client cli(parsed.host, parsed.port);
        apply_client_timeouts(cli, eff);
        ok = cli.send(req, res, err);
    }
    if (!ok) {
        std::ostringstream oss;
        oss << "HttplibClient: post_sse transport error " << static_cast<int>(err);
        throw llm_http_error(0, "httplib", oss.str(), std::nullopt);
    }
    if (response_status < 200 || response_status >= 300) {
        std::string excerpt =
            error_body_accum.empty() ? truncate_body(res.body, 512) : truncate_body(error_body_accum, 512);
        throw llm_http_error(response_status, "httplib", excerpt,
                             parse_retry_after_sec(res.headers));
    }
}

HttplibLlmTransport::HttplibLlmTransport(std::shared_ptr<HttplibClient> client)
    : client_(client ? std::move(client) : std::make_shared<HttplibClient>()) {}

void HttplibLlmTransport::set_http_timeout_sec(int sec) {
    client_->set_timeout_sec(sec);
}

json HttplibLlmTransport::post_llm(const std::string& url, const json& body,
                                   const std::map<std::string, std::string>& headers,
                                   const std::string& provider) {
    return client_->post_llm(url, body, headers, provider);
}

void HttplibLlmTransport::post_sse(const std::string& url, const json& body,
                                   const std::map<std::string, std::string>& headers,
                                   const std::function<void(const std::string&, const json&)>& on_event,
                                   int timeout_sec, const std::string& /*provider*/) {
    client_->post_sse(url, body, headers, on_event, timeout_sec);
}

} // namespace agent_framework
