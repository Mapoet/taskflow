/**
 * @file httplib_http_client.hpp
 * @brief HTTP 客户端实现（cpp-httplib），供 AgentClient / HTTPAgentTransport 使用
 * @author Mapoet
 * @version 0.1
 * @date 2026-03-31
 */
#ifndef __AGENT_HTTPLIB_HTTP_CLIENT_H__
#define __AGENT_HTTPLIB_HTTP_CLIENT_H__

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include <agent/agent_client/agent_client.hpp>
#include <agent/core/types.hpp>

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

    /** 0 表示使用 cpp-httplib 默认宏；>0 则统一设置连接/读/写超时（秒） */
    void set_timeout_sec(int sec) { timeout_sec_ = sec; }

    json post(const std::string& url, const json& body,
              const std::map<std::string, std::string>& headers = {}) override;

    /**
     * @brief POST JSON-RPC 2.0 请求：与 post() 相同，但若 HTTP 非 2xx 且 body 为合法 JSON-RPC
     *        error 信封（含 jsonrpc 2.0 与 error 对象），则返回该 JSON 供映射为 A2aRpcException。
     */
    json post_json_rpc(const std::string& url, const json& body,
                       const std::map<std::string, std::string>& headers = {});

    json get(const std::string& url,
             const std::map<std::string, std::string>& headers = {}) override;

    void get_sse(const std::string& url,
                 const std::map<std::string, std::string>& headers,
                 const std::function<void(std::string_view chunk)>& on_chunk,
                 int timeout_sec,
                 const std::atomic<bool>* cancel_flag) override;

    /**
     * @brief POST JSON；失败时抛出 llm_http_error（含 HTTP 状态与 Retry-After）
     */
    json post_llm(const std::string& url, const json& body,
                  const std::map<std::string, std::string>& headers, const std::string& provider);

    /**
     * @brief POST JSON 并消费 SSE（text/event-stream）：对每个 data: 行解析 JSON 后回调
     * @param on_event 参数为 event 名（可能为空）、data JSON、SSE id（可能为空）；OpenAI 通常 event/id 为空
     * @param timeout_sec 覆盖连接/读写超时；<=0 使用 set_timeout_sec 或库默认宏
     */
    void post_sse(const std::string& url, const json& body,
                  const std::map<std::string, std::string>& headers,
                  const std::function<void(const std::string& event_name, const json& data,
                                           const std::string& event_id)>& on_event,
                  int timeout_sec = 0,
                  const std::function<bool()>& cancellation_requested = {});

private:
    int timeout_sec_ = 0;
};

/**
 * @brief LLM 专用 HTTP 抽象，便于单测注入 fake transport
 */
class LlmHttpTransport {
public:
    virtual ~LlmHttpTransport() = default;
    /** 作用于后续 post_llm；post_sse 的 timeout_sec 参数优先 */
    virtual void set_http_timeout_sec(int sec) { (void)sec; }
    virtual json post_llm(const std::string& url, const json& body,
                          const std::map<std::string, std::string>& headers,
                          const std::string& provider) = 0;
    virtual void post_sse(const std::string& url, const json& body,
                          const std::map<std::string, std::string>& headers,
                          const std::function<void(const std::string& event_name, const json& data)>& on_event,
                          int timeout_sec, const std::string& provider) = 0;
    virtual void post_sse_cancellable(
        const std::string& url, const json& body,
        const std::map<std::string, std::string>& headers,
        const std::function<void(const std::string& event_name, const json& data)>& on_event,
        int timeout_sec, const std::string& provider,
        const std::function<bool()>& cancellation_requested) {
        (void)cancellation_requested;
        post_sse(url, body, headers, on_event, timeout_sec, provider);
    }
};

/** 默认实现：委托 HttplibClient */
class HttplibLlmTransport : public LlmHttpTransport {
public:
    explicit HttplibLlmTransport(std::shared_ptr<HttplibClient> client = nullptr);

    void set_http_timeout_sec(int sec) override;

    json post_llm(const std::string& url, const json& body,
                  const std::map<std::string, std::string>& headers,
                  const std::string& provider) override;

    void post_sse(const std::string& url, const json& body,
                  const std::map<std::string, std::string>& headers,
                  const std::function<void(const std::string& event_name, const json& data)>& on_event,
                  int timeout_sec, const std::string& provider) override;
    void post_sse_cancellable(
        const std::string& url, const json& body,
        const std::map<std::string, std::string>& headers,
        const std::function<void(const std::string& event_name, const json& data)>& on_event,
        int timeout_sec, const std::string& provider,
        const std::function<bool()>& cancellation_requested) override;

private:
    std::shared_ptr<HttplibClient> client_;
};

} // namespace agent_framework

#endif // __AGENT_HTTPLIB_HTTP_CLIENT_H__
