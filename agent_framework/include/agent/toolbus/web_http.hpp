/**
 * @file web_http.hpp
 * @brief 内建 web_*：受控 HTTPS GET（SSRF 缓解、重定向、体上限）
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-04
 */
#ifndef __AGENT_WEB_HTTP_H__
#define __AGENT_WEB_HTTP_H__

#include <agent/core/types.hpp>

#include <cstdint>
#include <map>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace agent_framework {

/**
 * @brief HTTP CONNECT 上游代理（HTTPS_PROXY / HTTP_PROXY 链解析结果）。
 */
struct WebHttpUpstreamProxy {
    std::string host;
    int port = 0;
    std::string user;
    std::string pass;
    bool valid() const noexcept {
        return !host.empty() && port > 0 && port <= 65535;
    }
};

/**
 * @brief 从 HTTPS_PROXY / https_proxy / HTTP_PROXY / http_proxy 读取并解析（与 curl 一致）。
 * @return 解析成功且可用时为 true
 */
bool load_web_http_upstream_proxy(WebHttpUpstreamProxy& out);

/**
 * @brief 与 builtin-web-tools.md「立项锁定」一致的抓取配置（环境变量在实现内读取）。
 */
struct WebHttpConfig {
    int timeout_ms = 30000;
    int max_redirects = 10;
    std::size_t max_body_bytes = 2097152;
    std::string user_agent = "agent-framework-web-tools/1.0";
    bool allow_http = false;
    /** 仅供受信任的固定服务端点（如默认本机 SearXNG）；普通 web_fetch 保持 false。 */
    bool allow_loopback = false;
    /** 非空时：仅允许列表内主机名（仍须过 IP 黑名单） */
    std::vector<std::string> allow_hosts;
};

/**
 * @brief 从 AGENT_WEB_* 等环境变量加载默认 WebHttpConfig（allow_http 来自 AGENT_WEB_ALLOW_HTTP）。
 */
WebHttpConfig load_web_http_config_from_env();

/**
 * @brief HTTPS/HTTP GET 结果；失败时 error_code 非空。
 */
struct WebHttpResult {
    int status = 0;
    std::string final_url;
    std::string content_type;
    std::string body;
    bool truncated = false;
    /** 空表示成功 */
    std::string error_code;
    int error_http_status = 0;
    std::map<std::string, std::string> headers;
    int redirects = 0;
};

struct WebHttpRequest {
    std::string url;
    std::string method{"GET"};
    std::map<std::string, std::string> headers;
    std::string body;
    std::string content_type;
    bool follow_redirects{true};
    /** Optional streaming response sink. Returning false aborts transport. */
    std::function<bool(std::string_view)> response_sink;
    /** Cooperative cancellation checked for every received response chunk. */
    std::function<bool()> cancellation_requested;
};

/** Execute a policy-checked HTTP request. Sensitive headers must be injected by a trusted caller. */
WebHttpResult web_http_request(const WebHttpRequest& request, const WebHttpConfig& cfg);

/** Header names that cannot be supplied by untrusted tool arguments. */
bool web_http_sensitive_header(std::string_view name);

/**
 * @brief 对绝对 URL 执行 GET：每跳重定向后重新做 SSRF 校验；body 流式截断于 max_body_bytes。
 * @param url 必须以 http:// 或 https:// 开头（或由 allow_http 决定 https-only）
 * @param extra_headers 可选附加头（如 Accept、Cookie）；若未含 Cookie 且设置了 AGENT_WEB_HTTP_COOKIE，实现会自动附加。
 */
WebHttpResult web_http_get(const std::string& url, const WebHttpConfig& cfg,
                           const std::map<std::string, std::string>& extra_headers = {});

json web_tool_error(const std::string& code, const std::string& message = {});

} // namespace agent_framework

#endif // __AGENT_WEB_HTTP_H__
