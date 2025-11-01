/**
 * @file mcp_client.hpp
 * @brief MCP 客户端模块：Model Context Protocol 客户端实现
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_MCP_CLIENT_H__
#define __AGENT_MCP_CLIENT_H__

#include "types.hpp"
#include <string>
#include <vector>
#include <memory>
#include <future>
#include <mutex>

// 前向声明
namespace websocketpp {
    namespace config {
        namespace asio {
            class message;
        }
    }
    template<typename Config>
    class connection_hdl;
}

namespace agent_framework {

// ============================================================================
// MCP 传输层接口
// ============================================================================

/**
 * @brief MCP 传输层虚基类
 * 定义统一的传输接口，支持不同的传输方式
 */
class MCPTransportInterface {
public:
    virtual ~MCPTransportInterface() = default;
    
    /**
     * @brief 连接服务
     * @param endpoint 端点（命令、URL 等）
     * @return true 如果连接成功
     */
    virtual bool connect(const std::string& endpoint) = 0;
    
    /**
     * @brief 断开连接
     */
    virtual void disconnect() = 0;
    
    /**
     * @brief 发送 JSON-RPC 请求
     * @param method 方法名
     * @param params 参数（JSON 格式）
     * @return 响应（JSON 格式）
     */
    virtual json send_request(const std::string& method, const json& params) = 0;
    
    /**
     * @brief 检查连接状态
     * @return true 如果已连接
     */
    virtual bool is_connected() const = 0;
    
    /**
     * @brief 获取传输类型
     * @return 传输类型枚举值
     */
    virtual MCPTransport get_transport_type() const = 0;
};

/**
 * @brief stdio 传输实现（标准输入输出）
 */
class StdioMCPTransport : public MCPTransportInterface {
public:
    /**
     * @brief 构造函数
     * @param command 命令（程序路径）
     * @param args 命令行参数
     */
    explicit StdioMCPTransport(const std::string& command, 
                               const std::vector<std::string>& args = {});
    
    bool connect(const std::string& endpoint) override;
    void disconnect() override;
    json send_request(const std::string& method, const json& params) override;
    bool is_connected() const override;
    MCPTransport get_transport_type() const override;
    
private:
    std::string command_;
    std::vector<std::string> args_;
    std::unique_ptr<void> process_;  // 子进程句柄（实际类型取决于平台）
    bool connected_ = false;
    std::mutex io_mutex_;
    
    /**
     * @brief 启动子进程
     */
    void start_process();
};

/**
 * @brief HTTP 传输实现
 */
class HttpMCPTransport : public MCPTransportInterface {
public:
    /**
     * @brief 构造函数
     * @param base_url 基础 URL
     */
    explicit HttpMCPTransport(const std::string& base_url);
    
    bool connect(const std::string& endpoint) override;
    void disconnect() override;
    json send_request(const std::string& method, const json& params) override;
    bool is_connected() const override;
    MCPTransport get_transport_type() const override;
    
private:
    std::string base_url_;
    std::string endpoint_;
    bool connected_ = false;
    
    /**
     * @brief 发送 HTTP POST 请求
     * @param payload 请求载荷（JSON 格式）
     * @return 响应（JSON 格式）
     */
    json send_http_post(const json& payload);
};

/**
 * @brief WebSocket 传输实现
 */
class WebSocketMCPTransport : public MCPTransportInterface {
public:
    /**
     * @brief 构造函数
     * @param ws_url WebSocket URL
     */
    explicit WebSocketMCPTransport(const std::string& ws_url);
    
    bool connect(const std::string& endpoint) override;
    void disconnect() override;
    json send_request(const std::string& method, const json& params) override;
    bool is_connected() const override;
    MCPTransport get_transport_type() const override;
    
private:
    std::string ws_url_;
    void* connection_;  // WebSocket 连接句柄（实际类型取决于 websocketpp）
    bool connected_ = false;
    std::mutex ws_mutex_;
    
    /**
     * @brief 处理 WebSocket 消息
     * @param hdl 连接句柄
     * @param msg 消息指针
     */
    void handle_message(void* hdl, void* msg);
};

// ============================================================================
// MCP 客户端管理器
// ============================================================================

/**
 * @brief MCP 客户端管理器（使用传输层）
 */
class MCPClient {
public:
    /**
     * @brief 构造函数
     * @param transport 传输层实例
     */
    explicit MCPClient(std::unique_ptr<MCPTransportInterface> transport);
    
    /**
     * @brief 连接 MCP 服务
     * @param endpoint 端点
     * @param transport_type 传输类型
     * @return true 如果连接成功
     */
    bool connect(const std::string& endpoint, MCPTransport transport_type);
    
    /**
     * @brief 列举可用工具
     * @return 工具元数据列表（异步 future）
     */
    std::future<std::vector<ToolMeta>> list_tools();
    
    /**
     * @brief 调用工具
     * @param name 工具名称
     * @param arguments 调用参数（JSON 格式）
     * @return 工具执行结果（JSON 格式，异步 future）
     */
    std::future<json> call_tool(const std::string& name, const json& arguments);
    
    /**
     * @brief 心跳检查
     * @return true 如果服务可用
     */
    bool ping();
    
    /**
     * @brief 断开连接
     */
    void disconnect();
    
    /**
     * @brief 检查连接状态
     * @return true 如果已连接
     */
    bool is_connected() const;
    
private:
    std::unique_ptr<MCPTransportInterface> transport_;
    std::vector<ToolMeta> cached_tools_;
    std::mutex cache_mutex_;
    
    /**
     * @brief 发送 JSON-RPC 2.0 请求
     * @param method 方法名
     * @param params 参数（JSON 格式）
     * @return 响应（JSON 格式）
     */
    json send_jsonrpc_request(const std::string& method, const json& params);
    
    /**
     * @brief 解析 JSON-RPC 2.0 响应
     * @param response 响应（JSON 格式）
     * @return 解析后的结果（JSON 格式）
     */
    json parse_jsonrpc_response(const json& response);
};

} // namespace agent_framework

#endif // __AGENT_MCP_CLIENT_H__

