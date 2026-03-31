/**
 * @file httplib_http_client.hpp
 * @brief HTTP 客户端实现（cpp-httplib），供 AgentClient / HTTPAgentTransport 使用
 * @author Mapoet
 * @version 0.1
 * @date 2026-03-31
 */
#ifndef __AGENT_HTTPLIB_HTTP_CLIENT_H__
#define __AGENT_HTTPLIB_HTTP_CLIENT_H__

#include <map>
#include <string>

#include <agent/agent_client.hpp>

namespace agent_framework {

/**
 * @brief 基于 cpp-httplib 的 HTTPClient 实现（GET/POST JSON）
 *
 * - 支持绝对 URL（http:// 或 https://，后者需编译时启用 CPPHTTPLIB_OPENSSL_SUPPORT 并链接 OpenSSL）
 * - 非 2xx 或连接失败时抛出 std::runtime_error
 */
class HttplibClient : public HTTPClient {
public:
    HttplibClient() = default;

    json post(const std::string& url, const json& body,
              const std::map<std::string, std::string>& headers = {}) override;

    json get(const std::string& url,
             const std::map<std::string, std::string>& headers = {}) override;
};

} // namespace agent_framework

#endif // __AGENT_HTTPLIB_HTTP_CLIENT_H__
