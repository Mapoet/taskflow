/**
 * @file mcp_client.hpp
 * @brief MCP 客户端：JSON-RPC 2.0、stdio/HTTP 传输（WP1.3）
 */
#ifndef __AGENT_MCP_CLIENT_H__
#define __AGENT_MCP_CLIENT_H__

#include <agent/core/types.hpp>

#include <atomic>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agent_framework {

class MCPTransportInterface {
public:
    virtual ~MCPTransportInterface() = default;
    virtual bool connect(const std::string& endpoint) = 0;
    virtual void disconnect() = 0;
    /** JSON-RPC 请求（含 id）；返回完整响应 JSON */
    virtual json transceive(const json& jsonrpc_request) = 0;
    virtual json transceive_cancellable(const json& jsonrpc_request,
                                        const std::function<bool()>& cancellation_requested) {
        if (cancellation_requested && cancellation_requested()) {
            throw std::runtime_error("MCP request cancelled");
        }
        return transceive(jsonrpc_request);
    }
    /** JSON-RPC 通知（无 id）；不期待响应 */
    virtual void send_notification(const json& jsonrpc_notification) = 0;
    virtual bool is_connected() const = 0;
    virtual MCPTransport get_transport_type() const = 0;
};

/**
 * @brief stdio 传输（Content-Length 帧，见 mcp-spec-tracker.md）
 */
class StdioMCPTransport : public MCPTransportInterface {
public:
    explicit StdioMCPTransport(std::string command, std::vector<std::string> args = {},
                               std::map<std::string, std::string> extra_env = {});
    ~StdioMCPTransport() override;

    StdioMCPTransport(const StdioMCPTransport&) = delete;
    StdioMCPTransport& operator=(const StdioMCPTransport&) = delete;

    bool connect(const std::string& endpoint) override;
    void disconnect() override;
    json transceive(const json& jsonrpc_request) override;
    json transceive_cancellable(const json& jsonrpc_request,
                                const std::function<bool()>& cancellation_requested) override;
    void send_notification(const json& jsonrpc_notification) override;
    bool is_connected() const override;
    MCPTransport get_transport_type() const override;

private:
    struct StdioPipes;
    std::unique_ptr<StdioPipes> io_;
    std::string command_;
    std::vector<std::string> args_;
    std::map<std::string, std::string> extra_env_;
    bool connected_ = false;
    std::mutex io_mutex_;
    std::string pending_read_;

    void write_framed_message(const json& msg);
    json read_framed_message();
    json read_framed_message(const std::function<bool()>& cancellation_requested);
};

/**
 * @brief HTTP 传输：对 post_url 整段 POST JSON-RPC
 */
class HttpMCPTransport : public MCPTransportInterface {
public:
    explicit HttpMCPTransport(std::string post_url,
                              std::map<std::string, std::string> extra_headers = {});

    bool connect(const std::string& endpoint) override;
    void disconnect() override;
    json transceive(const json& jsonrpc_request) override;
    json transceive_cancellable(const json& jsonrpc_request,
                                const std::function<bool()>& cancellation_requested) override;
    void send_notification(const json& jsonrpc_notification) override;
    bool is_connected() const override;
    MCPTransport get_transport_type() const override;

private:
    std::string post_url_;
    std::map<std::string, std::string> headers_;
    bool connected_ = false;
    std::mutex io_mutex_;

    json post_json(const json& body, const std::function<bool()>& cancellation_requested = {});
};

class WebSocketMCPTransport : public MCPTransportInterface {
public:
    explicit WebSocketMCPTransport(const std::string& ws_url);
    ~WebSocketMCPTransport() override;

    bool connect(const std::string& endpoint) override;
    void disconnect() override;
    json transceive(const json& jsonrpc_request) override;
    void send_notification(const json& jsonrpc_notification) override;
    bool is_connected() const override;
    MCPTransport get_transport_type() const override;

private:
    std::string ws_url_;
    void* connection_ = nullptr;
    bool connected_ = false;
    std::mutex ws_mutex_;
    void handle_message(void* hdl, void* msg);
};

/**
 * @brief MCP 客户端（工厂创建；握手后可用）
 */
class MCPClient {
public:
    /** 启动子进程 stdio MCP 并完成 initialize */
    static std::shared_ptr<MCPClient> create_stdio(const std::string& command,
                                                   const std::vector<std::string>& args = {});
    static std::shared_ptr<MCPClient> create_stdio(
        const std::string& command, const std::vector<std::string>& args,
        const std::map<std::string, std::string>& extra_env);

    /** HTTP POST 到 post_url（完整 URL），可选额外头 */
    static std::shared_ptr<MCPClient> create_http(
        const std::string& post_url,
        const std::map<std::string, std::string>& extra_headers = {});

    /**
     * @brief 自定义传输（测试/扩展）：connect("") 成功后可选执行握手
     */
    static std::shared_ptr<MCPClient> create_with_transport(
        std::unique_ptr<MCPTransportInterface> transport, bool run_handshake = true);

    ~MCPClient();

    MCPClient(const MCPClient&) = delete;
    MCPClient& operator=(const MCPClient&) = delete;

    std::future<std::vector<ToolMeta>> list_tools();
    std::future<json> call_tool(const std::string& name, const json& arguments,
                                std::function<bool()> cancellation_requested = {});
    bool ping();
    void disconnect();
    bool is_connected() const;

    /** 单元测试：解析 JSON-RPC 响应（expected_id 须与请求 id 一致） */
    static json parse_jsonrpc_response(const json& response, std::int64_t expected_id);

private:
    explicit MCPClient(std::unique_ptr<MCPTransportInterface> transport);
    void handshake();

    json send_jsonrpc_request(const std::string& method, const json& params,
                              const std::function<bool()>& cancellation_requested = {});

    std::unique_ptr<MCPTransportInterface> transport_;
    std::vector<ToolMeta> cached_tools_;
    std::mutex cache_mutex_;
    std::mutex rpc_mutex_;
    std::atomic<std::int64_t> next_id_{1};
};

} // namespace agent_framework

#endif // __AGENT_MCP_CLIENT_H__
