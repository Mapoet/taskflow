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

void apply_timeouts_for_sec(httplib::Client& cli, int sec) {
    if (sec > 0) {
        cli.set_connection_timeout(sec, 0);
        cli.set_read_timeout(sec, 0);
        cli.set_write_timeout(sec, 0);
    } else {
        apply_default_timeouts(cli);
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
    httplib::Client cli(parsed.scheme_host_port.c_str());
    if (!cli.is_valid()) {
        throw llm_http_error(0, provider, "HttplibClient: invalid client for " + parsed.scheme_host_port,
                             std::nullopt);
    }
    apply_timeouts_for_sec(cli, timeout_sec_);

    httplib::Headers h = to_httplib_headers(headers);
    const std::string payload = body.dump();
    auto result = cli.Post(parsed.path_and_query.c_str(), h, payload, "application/json");
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
    httplib::Client cli(parsed.scheme_host_port.c_str());
    if (!cli.is_valid()) {
        throw std::runtime_error("HttplibClient: invalid client for URL: " + parsed.scheme_host_port);
    }
    apply_timeouts_for_sec(cli, timeout_sec_);

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
    apply_timeouts_for_sec(cli, timeout_sec_);

    httplib::Headers h = to_httplib_headers(headers);
    auto result = cli.Get(parsed.path_and_query.c_str(), h);
    return execute_and_parse_json(result, "GET");
}

void HttplibClient::post_sse(const std::string& url, const json& body,
                             const std::map<std::string, std::string>& headers,
                             const std::function<void(const std::string& event_name, const json& data)>&
                                 on_event,
                             int timeout_sec) {
    ParsedHttpUrl parsed = parse_absolute_url(url);
    httplib::Client cli(parsed.scheme_host_port.c_str());
    if (!cli.is_valid()) {
        throw std::runtime_error("HttplibClient: invalid client for URL: " + parsed.scheme_host_port);
    }
    const int eff = timeout_sec > 0 ? timeout_sec : timeout_sec_;
    apply_timeouts_for_sec(cli, eff);

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
    const bool ok = cli.send(req, res, err);
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
