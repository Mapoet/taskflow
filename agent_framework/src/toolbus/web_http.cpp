/**
 * @file web_http.cpp
 * @brief 受控 HTTP(S) GET：SSRF、重定向、流式体上限；HTTPS_PROXY/HTTP_PROXY（HTTP CONNECT）、AGENT_WEB_HTTP_COOKIE。
 */

#include <agent/toolbus/web_http.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string_view>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#if __has_include(<httplib/httplib.hpp>)
#include <httplib/httplib.hpp>
#elif __has_include(<httplib.hpp>)
#include <httplib.hpp>
#elif __has_include(<httplib.h>)
#include <httplib.h>
#else
#error "httplib not found for web_http"
#endif

namespace agent_framework {
namespace {

void trim_inplace(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
}

bool env_truthy(const char* v) {
    if (!v || !*v) {
        return false;
    }
    std::string s(v);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

int env_int(const char* key, int def_v) {
    const char* v = std::getenv(key);
    if (!v || !*v) {
        return def_v;
    }
    return std::atoi(v);
}

std::size_t env_size(const char* key, std::size_t def_v) {
    const char* v = std::getenv(key);
    if (!v || !*v) {
        return def_v;
    }
    const long n = std::strtol(v, nullptr, 10);
    if (n <= 0) {
        return def_v;
    }
    return static_cast<std::size_t>(n);
}

std::string env_str(const char* key, const std::string& def_v) {
    const char* v = std::getenv(key);
    if (v && *v) {
        return std::string(v);
    }
    return def_v;
}

struct ParsedUrl {
    std::string scheme;
    std::string host;
    int port = 0;
    std::string path_and_query;
};

std::optional<ParsedUrl> parse_url_components(const std::string& url_raw) {
    std::string url = url_raw;
    trim_inplace(url);
    if (url.empty()) {
        return std::nullopt;
    }
    if (url.size() > 8192) {
        return std::nullopt;
    }
    ParsedUrl o;
    std::size_t pos = 0;
    if (url.rfind("https://", 0) == 0) {
        o.scheme = "https";
        pos = 8;
    } else if (url.rfind("http://", 0) == 0) {
        o.scheme = "http";
        pos = 7;
    } else {
        return std::nullopt;
    }

    const std::size_t path_i = url.find('/', pos);
    const std::size_t host_end = (path_i == std::string::npos) ? url.size() : path_i;
    std::string authority = url.substr(pos, host_end - pos);
    if (authority.empty()) {
        return std::nullopt;
    }

    int port = (o.scheme == "https") ? 443 : 80;
    std::string host = authority;
    if (authority[0] == '[') {
        const std::size_t br = authority.find(']');
        if (br == std::string::npos) {
            return std::nullopt;
        }
        host = authority.substr(1, br - 1);
        if (br + 1 < authority.size() && authority[br + 1] == ':') {
            port = std::atoi(authority.c_str() + static_cast<int>(br) + 2);
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string::npos && authority.find(':') == colon) {
            host = authority.substr(0, colon);
            port = std::atoi(authority.c_str() + static_cast<int>(colon) + 1);
        }
    }
    if (host.empty() || port <= 0 || port > 65535) {
        return std::nullopt;
    }
    o.host = host;
    o.port = port;
    if (path_i == std::string::npos) {
        o.path_and_query = "/";
    } else {
        o.path_and_query = url.substr(path_i);
        if (o.path_and_query.empty()) {
            o.path_and_query = "/";
        }
    }
    return o;
}

bool ipv4_in_blocked_range(std::uint32_t ip_be) {
    const unsigned char* b = reinterpret_cast<const unsigned char*>(&ip_be);
    const unsigned a = b[0];
    const unsigned b1 = b[1];
    const unsigned c = b[2];
    const unsigned d = b[3];
    (void)c;
    (void)d;
    if (a == 127U) {
        return true;
    }
    if (a == 10U) {
        return true;
    }
    if (a == 172U && b1 >= 16U && b1 <= 31U) {
        return true;
    }
    if (a == 192U && b1 == 168U) {
        return true;
    }
    if (a == 169U && b1 == 254U) {
        return true;
    }
    if (a == 0U) {
        return true;
    }
    return false;
}

bool sockaddr_blocked(const struct sockaddr* sa) {
    if (sa->sa_family == AF_INET) {
        auto* in = reinterpret_cast<const struct sockaddr_in*>(sa);
        return ipv4_in_blocked_range(in->sin_addr.s_addr);
    }
    if (sa->sa_family == AF_INET6) {
        auto* in6 = reinterpret_cast<const struct sockaddr_in6*>(sa);
        const unsigned char* x = in6->sin6_addr.s6_addr;
        bool all_zero15 = true;
        for (int i = 0; i < 15; ++i) {
            if (x[i] != 0) {
                all_zero15 = false;
                break;
            }
        }
        if (all_zero15 && x[15] == 1) {
            return true;
        }
        if (x[0] == 0xfe && (x[1] & 0xc0U) == 0x80U) {
            return true;
        }
        if (x[0] == 0xfc || x[0] == 0xfd) {
            return true;
        }
        if (x[0] == 0 && x[1] == 0 && x[2] == 0 && x[3] == 0 && x[4] == 0 && x[5] == 0 && x[6] == 0 && x[7] == 0 &&
            x[8] == 0 && x[9] == 0 && x[10] == 0xff && x[11] == 0xff) {
            std::uint32_t v4;
            std::memcpy(&v4, x + 12, 4);
            return ipv4_in_blocked_range(v4);
        }
    }
    return false;
}

bool literal_ip_host_blocked(const std::string& host) {
    struct in_addr a4 {};
    if (inet_pton(AF_INET, host.c_str(), &a4) == 1) {
        std::uint32_t be = a4.s_addr;
        return ipv4_in_blocked_range(be);
    }
    struct in6_addr a6 {};
    if (inet_pton(AF_INET6, host.c_str(), &a6) == 1) {
        struct sockaddr_in6 tmp {};
      tmp.sin6_family = AF_INET6;
        tmp.sin6_addr = a6;
        return sockaddr_blocked(reinterpret_cast<struct sockaddr*>(&tmp));
    }
    return false;
}

bool host_passes_allowlist(const std::string& host_lower, const std::vector<std::string>& allow) {
    if (allow.empty()) {
        return true;
    }
    for (const auto& h : allow) {
        std::string hl = h;
        for (char& c : hl) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (hl == host_lower) {
            return true;
        }
    }
    return false;
}

bool addrs_all_loopback(const std::string& host) {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
        freeaddrinfo(res);
        return false;
    }
    bool any = false;
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        if (!p->ai_addr) {
            continue;
        }
        any = true;
        if (p->ai_addr->sa_family == AF_INET) {
            auto* in = reinterpret_cast<struct sockaddr_in*>(p->ai_addr);
            if ((ntohl(in->sin_addr.s_addr) >> 24) != 127U) {
                freeaddrinfo(res);
                return false;
            }
        } else if (p->ai_addr->sa_family == AF_INET6) {
            auto* in6 = reinterpret_cast<struct sockaddr_in6*>(p->ai_addr);
            const unsigned char* x = in6->sin6_addr.s6_addr;
            bool az = true;
            for (int i = 0; i < 15; ++i) {
                if (x[i] != 0) {
                    az = false;
                    break;
                }
            }
            if (!(az && x[15] == 1)) {
                freeaddrinfo(res);
                return false;
            }
        } else {
            freeaddrinfo(res);
            return false;
        }
    }
    freeaddrinfo(res);
    return any;
}

bool resolve_host_safe_for_ssrf(const std::string& host, const WebHttpConfig& cfg) {
    std::string hlow = host;
    for (char& c : hlow) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (!host_passes_allowlist(hlow, cfg.allow_hosts)) {
        return false;
    }
    if (cfg.allow_loopback || env_truthy(std::getenv("AGENT_WEB_TEST_ALLOW_LOOPBACK"))) {
        if (addrs_all_loopback(host)) {
            return true;
        }
    }
    if (literal_ip_host_blocked(host)) {
        return false;
    }

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    const int gai = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (gai != 0 || res == nullptr) {
        freeaddrinfo(res);
        return false;
    }
    bool ok = true;
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        if (p->ai_addr && sockaddr_blocked(p->ai_addr)) {
            ok = false;
            break;
        }
    }
    freeaddrinfo(res);
    return ok;
}

bool url_passes_ssrf(const std::string& url, const WebHttpConfig& cfg, std::string& err_code) {
    const auto pu = parse_url_components(url);
    if (!pu) {
        err_code = "invalid_url";
        return false;
    }
    if (pu->scheme == "http" && !cfg.allow_http) {
        err_code = "url_disallowed";
        return false;
    }
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    if (pu->scheme == "https") {
        err_code = "url_disallowed";
        return false;
    }
#endif
    if (!resolve_host_safe_for_ssrf(pu->host, cfg)) {
        err_code = "url_disallowed";
        return false;
    }
    err_code.clear();
    return true;
}

void split_allow_hosts_env(std::vector<std::string>& out) {
    const char* raw = std::getenv("AGENT_WEB_ALLOW_HOSTS");
    if (!raw || !*raw) {
        return;
    }
    std::string chunk;
    for (const char* p = raw; *p != '\0'; ++p) {
        if (*p == ',') {
            trim_inplace(chunk);
            if (!chunk.empty()) {
                out.push_back(chunk);
                chunk.clear();
            }
        } else {
            chunk.push_back(*p);
        }
    }
    trim_inplace(chunk);
    if (!chunk.empty()) {
        out.push_back(std::move(chunk));
    }
}

std::string merge_relative_url(const std::string& base_url, const std::string& location) {
    std::string loc = location;
    trim_inplace(loc);
    if (loc.empty()) {
        return base_url;
    }
    if (loc.rfind("http://", 0) == 0 || loc.rfind("https://", 0) == 0) {
        return loc;
    }
    const auto base = parse_url_components(base_url);
    if (!base) {
        return {};
    }
    if (!loc.empty() && loc[0] == '/') {
        return base->scheme + "://" + base->host +
               (base->port != (base->scheme == "https" ? 443 : 80)
                    ? (":" + std::to_string(base->port))
                    : std::string()) +
               loc;
    }
    std::string path = base->path_and_query;
    const std::size_t q = path.find('?');
    if (q != std::string::npos) {
        path = path.substr(0, q);
    }
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        path = "/";
    } else {
        path = path.substr(0, slash + 1);
    }
    return base->scheme + "://" + base->host +
           (base->port != (base->scheme == "https" ? 443 : 80) ? (":" + std::to_string(base->port))
                                                                : std::string()) +
           path + loc;
}

template <typename ClientLike>
void apply_client_timeouts(ClientLike& cli, int timeout_ms) {
    if (timeout_ms <= 0) {
        timeout_ms = 30000;
    }
    const time_t sec = static_cast<time_t>(timeout_ms / 1000);
    const time_t usec = static_cast<time_t>((timeout_ms % 1000) * 1000);
    cli.set_connection_timeout(sec, usec);
    cli.set_read_timeout(sec, usec);
    cli.set_write_timeout(sec, usec);
}

bool tolower_prefix_match_sv(std::string_view s, std::string_view pref) {
    if (s.size() < pref.size()) {
        return false;
    }
    for (std::size_t i = 0; i < pref.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(pref[i]))) {
            return false;
        }
    }
    return true;
}

/**
 * 解析 http(s)://host:port、host:port、user:pass@host:port、[::1]:port；不支持 socks5://。
 */
bool parse_upstream_proxy_url(std::string s, WebHttpUpstreamProxy& out) {
    trim_inplace(s);
    if (s.empty()) {
        return false;
    }
    {
        std::string low;
        low.reserve(s.size());
        for (char c : s) {
            low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (low == "none" || low == "off" || low == "false" || low == "disable") {
            return false;
        }
    }
    while (tolower_prefix_match_sv(s, "https://")) {
        s.erase(0, 8);
    }
    while (tolower_prefix_match_sv(s, "http://")) {
        s.erase(0, 7);
    }
    trim_inplace(s);
    if (s.empty()) {
        return false;
    }

    const std::size_t at = s.find('@');
    if (at != std::string::npos) {
        std::string auth = s.substr(0, at);
        s = s.substr(at + 1);
        trim_inplace(s);
        const std::size_t ac = auth.find(':');
        if (ac != std::string::npos) {
            out.user = auth.substr(0, ac);
            out.pass = auth.substr(ac + 1);
        } else {
            out.user = std::move(auth);
        }
        trim_inplace(out.user);
        trim_inplace(out.pass);
    }

    if (!s.empty() && s.front() == '[') {
        const std::size_t br = s.find(']');
        if (br == std::string::npos || br < 2) {
            return false;
        }
        out.host = s.substr(1, br - 1);
        if (br + 1 < s.size() && s[br + 1] == ':') {
            const std::string ps = s.substr(br + 2);
            if (ps.empty()) {
                return false;
            }
            for (char c : ps) {
                if (!std::isdigit(static_cast<unsigned char>(c))) {
                    return false;
                }
            }
            out.port = std::atoi(ps.c_str());
        } else {
            out.port = 8080;
        }
        return !out.host.empty() && out.port > 0 && out.port <= 65535;
    }

    const std::size_t colon = s.rfind(':');
    if (colon != std::string::npos && colon + 1 < s.size()) {
        const std::string port_str = s.substr(colon + 1);
        bool all_digit = true;
        for (char c : port_str) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                all_digit = false;
                break;
            }
        }
        if (all_digit && !port_str.empty()) {
            out.host = s.substr(0, colon);
            trim_inplace(out.host);
            out.port = std::atoi(port_str.c_str());
            return !out.host.empty() && out.port > 0 && out.port <= 65535;
        }
    }

    out.host = s;
    trim_inplace(out.host);
    out.port = 8080;
    return !out.host.empty();
}

const char* upstream_proxy_env_raw() {
    static const char* const keys[] = {"HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy"};
    for (const char* k : keys) {
        const char* v = std::getenv(k);
        if (v && *v) {
            return v;
        }
    }
    return nullptr;
}

} // namespace

bool load_web_http_upstream_proxy(WebHttpUpstreamProxy& out) {
    out = WebHttpUpstreamProxy{};
    const char* raw = upstream_proxy_env_raw();
    if (!raw) {
        return false;
    }
    return parse_upstream_proxy_url(std::string(raw), out);
}

bool web_http_extra_headers_has_cookie(const std::map<std::string, std::string>& m) {
    for (const auto& kv : m) {
        std::string k = kv.first;
        for (char& c : k) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (k == "cookie") {
            return true;
        }
    }
    return false;
}

template <typename ClientLike>
void apply_upstream_proxy_to_httplib_client(ClientLike& cli) {
    WebHttpUpstreamProxy px;
    if (!load_web_http_upstream_proxy(px) || !px.valid()) {
        return;
    }
    cli.set_proxy(px.host.c_str(), px.port);
    if (!px.user.empty()) {
        cli.set_proxy_basic_auth(px.user.c_str(), px.pass.c_str());
    }
}

WebHttpConfig load_web_http_config_from_env() {
    WebHttpConfig c;
    c.timeout_ms = env_int("AGENT_WEB_TIMEOUT_MS", 30000);
    c.max_redirects = env_int("AGENT_WEB_MAX_REDIRECTS", 10);
    c.max_body_bytes = env_size("AGENT_WEB_MAX_RESPONSE_BYTES", 2097152);
    c.user_agent = env_str("AGENT_WEB_USER_AGENT", "agent-framework-web-tools/1.0");
    c.allow_http = env_truthy(std::getenv("AGENT_WEB_ALLOW_HTTP"));
    split_allow_hosts_env(c.allow_hosts);
    return c;
}

json web_tool_error(const std::string& code, const std::string& message) {
    json e = json::object();
    e["error"] = json{{"code", code}};
    if (!message.empty()) {
        e["error"]["message"] = message;
    }
    return e;
}

WebHttpResult web_http_get(const std::string& url_in, const WebHttpConfig& cfg,
                           const std::map<std::string, std::string>& extra_headers) {
    WebHttpResult out;
    std::string current = url_in;
    int redirects = 0;
    while (true) {
        std::string ec;
        if (!url_passes_ssrf(current, cfg, ec)) {
            out.error_code = ec.empty() ? "url_disallowed" : ec;
            return out;
        }
        const auto pu = parse_url_components(current);
        if (!pu) {
            out.error_code = "invalid_url";
            return out;
        }

        httplib::Headers headers;
        headers.emplace("User-Agent", cfg.user_agent);
        for (const auto& kv : extra_headers) {
            headers.emplace(kv.first, kv.second);
        }
        if (!web_http_extra_headers_has_cookie(extra_headers)) {
            const std::string ck = env_str("AGENT_WEB_HTTP_COOKIE", "");
            if (!ck.empty()) {
                headers.emplace("Cookie", ck);
            }
        }

        std::string body;
        bool truncated = false;
        auto on_data = [&body, &truncated, maxb = cfg.max_body_bytes](const char* data,
                                                                      std::size_t len) -> bool {
            if (len == 0) {
                return true;
            }
            const std::size_t room = maxb > body.size() ? (maxb - body.size()) : 0U;
            if (room == 0U) {
                truncated = true;
                return false;
            }
            const std::size_t take = std::min(room, len);
            body.append(data, take);
            if (take < len) {
                truncated = true;
                return false;
            }
            return true;
        };

        std::optional<httplib::Result> res_opt;
        if (pu->scheme == "https") {
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
            out.error_code = "url_disallowed";
            return out;
#else
            httplib::SSLClient cli(pu->host, pu->port);
            apply_client_timeouts(cli, cfg.timeout_ms);
            cli.set_follow_location(false);
            apply_upstream_proxy_to_httplib_client(cli);
            res_opt = cli.Get(pu->path_and_query.c_str(), headers, on_data);
#endif
        } else {
            httplib::Client cli(pu->host, pu->port);
            apply_client_timeouts(cli, cfg.timeout_ms);
            cli.set_follow_location(false);
            apply_upstream_proxy_to_httplib_client(cli);
            res_opt = cli.Get(pu->path_and_query.c_str(), headers, on_data);
        }
        httplib::Result res = std::move(*res_opt);

        if (!res) {
            out.error_code = "timeout";
            return out;
        }

        out.status = res->status;
        out.final_url = current;
        const auto ct = res->get_header_value("Content-Type");
        if (!ct.empty()) {
            out.content_type = ct;
        }
        out.body = std::move(body);
        out.truncated = truncated;

        if (res->status == 301 || res->status == 302 || res->status == 303 || res->status == 307 ||
            res->status == 308) {
            if (++redirects > cfg.max_redirects) {
                out.error_code = "too_many_redirects";
                out.body.clear();
                return out;
            }
            std::string loc = res->get_header_value("Location");
            trim_inplace(loc);
            if (loc.empty()) {
                out.error_code = "http_error";
                out.error_http_status = res->status;
                return out;
            }
            std::string next = merge_relative_url(current, loc);
            if (next.empty()) {
                out.error_code = "invalid_url";
                out.body.clear();
                return out;
            }
            current = std::move(next);
            out = WebHttpResult{};
            continue;
        }

        if (res->status < 200 || res->status >= 300) {
            out.error_code = "http_error";
            out.error_http_status = res->status;
            return out;
        }
        return out;
    }
}

} // namespace agent_framework
