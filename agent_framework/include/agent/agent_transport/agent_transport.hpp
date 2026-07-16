/**
 * @file agent_transport.hpp
 * @brief Agent 传输层接口（A2A 协议）
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_TRANSPORT_H__
#define __AGENT_TRANSPORT_H__

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <agent/agent_client/agent_client.hpp>
#include <nlohmann/json.hpp>

namespace agent_framework {

using json = nlohmann::json;

/**
 * @brief Agent 传输层虚基类（A2A 协议）
 * 定义统一的传输接口，支持 HTTP、WebSocket 等不同传输方式
 */
class AgentTransport {
public:
    virtual ~AgentTransport() = default;

    /**
     * @brief 连接服务
     * @param endpoint 服务端点 URL
     * @return 是否连接成功
     */
    virtual bool connect(const std::string& endpoint) = 0;

    /**
     * @brief 断开连接
     */
    virtual void disconnect() = 0;

    /**
     * @brief 发送 JSON-RPC 2.0 请求
     * @param method 方法名
     * @param params 参数（JSON 对象）
     * @return 响应（JSON 对象）
     */
    virtual json send_request(const std::string& method, const json& params) = 0;

    /**
     * @brief 检查连接状态
     * @return 是否已连接
     */
    virtual bool is_connected() const = 0;

    /**
     * @brief 获取传输类型
     * @return 传输类型字符串（"http", "websocket" 等）
     */
    virtual std::string get_transport_type() const = 0;
};

/**
 * @brief HTTP Agent 传输实现（JSON-RPC 2.0 POST 至 base_url + endpoint）
 */
class HTTPAgentTransport : public AgentTransport {
public:
    /**
     * @brief 构造函数
     * @param base_url 基础 URL（如 "https://agent.example.com"）
     */
    explicit HTTPAgentTransport(const std::string& base_url);

    /**
     * @brief 析构函数
     */
    ~HTTPAgentTransport() override;

    /**
     * @brief 连接服务
     * @param endpoint 服务端点 URL（相对于 base_url）
     * @return 是否连接成功
     */
    bool connect(const std::string& endpoint) override;

    /**
     * @brief 断开连接
     */
    void disconnect() override;

    /**
     * @brief 发送 JSON-RPC 2.0 请求
     * @param method 方法名
     * @param params 参数（JSON 对象）
     * @return 响应（JSON 对象）
     */
    json send_request(const std::string& method, const json& params) override;

    /**
     * @brief 检查连接状态
     * @return 是否已连接
     */
    bool is_connected() const override;

    /**
     * @brief 获取传输类型
     * @return "http"
     */
    std::string get_transport_type() const override;

private:
    std::string base_url_;              // 基础 URL
    std::string current_endpoint_;      // 当前端点
    bool connected_ = false;            // 连接状态
    std::unique_ptr<HTTPClient> http_client_; // HttplibClient
    std::atomic<std::uint64_t> jsonrpc_next_id_{1};
};

} // namespace agent_framework

#endif // __AGENT_TRANSPORT_H__
