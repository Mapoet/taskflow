/**
 * @file toolbus.hpp
 * @brief ToolBus 模块：统一工具管理接口
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_TOOLBUS_H__
#define __AGENT_TOOLBUS_H__

#include "types.hpp"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <future>
#include <mutex>
#include <optional>

// 前向声明
namespace agent_framework {
    class MCPClient;
}

namespace agent_framework {

// ============================================================================
// 工具接口
// ============================================================================

/**
 * @brief 工具接口虚基类
 * 定义统一的工具调用接口，屏蔽不同工具实现的差异
 */
class ToolInterface {
public:
    virtual ~ToolInterface() = default;
    
    /**
     * @brief 调用工具
     * @param name 工具名称
     * @param arguments 调用参数（JSON 格式）
     * @return 工具执行结果（JSON 格式，异步 future）
     */
    virtual std::future<json> call(const std::string& name, const json& arguments) = 0;
    
    /**
     * @brief 获取工具元数据
     * @param name 工具名称
     * @return 工具元数据
     */
    virtual ToolMeta get_tool_meta(const std::string& name) const = 0;
    
    /**
     * @brief 列出所有可用工具
     * @return 工具名称列表
     */
    virtual std::vector<std::string> list_tools() const = 0;
    
    /**
     * @brief 验证参数
     * @param name 工具名称
     * @param arguments 参数（JSON 格式）
     * @return true 如果参数有效
     */
    virtual bool validate_arguments(const std::string& name, const json& arguments) const = 0;
    
    /**
     * @brief 获取工具信息
     * @param name 工具名称
     * @return 工具信息（如果存在）
     */
    virtual std::optional<ToolInfo> get_tool_info(const std::string& name) const = 0;
};

/**
 * @brief 本地工具实现
 */
class LocalTool : public ToolInterface {
public:
    /**
     * @brief 构造函数
     * @param name 工具名称
     * @param func 工具函数（接收 JSON 参数，返回 JSON 结果）
     * @param meta 工具元数据
     */
    LocalTool(const std::string& name, 
              std::function<json(const json&)> func,
              const ToolMeta& meta);
    
    std::future<json> call(const std::string& name, const json& arguments) override;
    ToolMeta get_tool_meta(const std::string& name) const override;
    std::vector<std::string> list_tools() const override;
    bool validate_arguments(const std::string& name, const json& arguments) const override;
    std::optional<ToolInfo> get_tool_info(const std::string& name) const override;
    
private:
    std::string name_;
    std::function<json(const json&)> func_;
    ToolMeta meta_;
    ToolInfo info_;
};

/**
 * @brief MCP 工具实现（通过 MCPClient 调用）
 */
class MCPTool : public ToolInterface {
public:
    /**
     * @brief 构造函数
     * @param client MCP 客户端
     */
    explicit MCPTool(std::shared_ptr<MCPClient> client);
    
    std::future<json> call(const std::string& name, const json& arguments) override;
    ToolMeta get_tool_meta(const std::string& name) const override;
    std::vector<std::string> list_tools() const override;
    bool validate_arguments(const std::string& name, const json& arguments) const override;
    std::optional<ToolInfo> get_tool_info(const std::string& name) const override;
    
private:
    std::shared_ptr<MCPClient> client_;
    std::vector<ToolMeta> cached_tools_;
    std::mutex cache_mutex_;
    
    /**
     * @brief 刷新工具列表缓存
     */
    void refresh_tools_cache();
};

/**
 * @brief API 工具实现（外部 REST API）
 */
class APITool : public ToolInterface {
public:
    /**
     * @brief 构造函数
     * @param name 工具名称
     * @param endpoint API 端点 URL
     * @param method HTTP 方法（"GET", "POST", "PUT", "DELETE"）
     * @param meta 工具元数据
     */
    APITool(const std::string& name,
            const std::string& endpoint,
            const std::string& method,
            const ToolMeta& meta);
    
    std::future<json> call(const std::string& name, const json& arguments) override;
    ToolMeta get_tool_meta(const std::string& name) const override;
    std::vector<std::string> list_tools() const override;
    bool validate_arguments(const std::string& name, const json& arguments) const override;
    std::optional<ToolInfo> get_tool_info(const std::string& name) const override;
    
private:
    std::string name_;
    std::string endpoint_;
    std::string method_;
    ToolMeta meta_;
    ToolInfo info_;
    
    /**
     * @brief 发送 HTTP 请求
     * @param payload 请求载荷（JSON 格式）
     * @return 响应（JSON 格式）
     */
    json send_http_request(const json& payload);
};

// ============================================================================
// ToolBus 管理器
// ============================================================================

/**
 * @brief ToolBus 管理器（统一管理所有工具）
 */
class ToolBus {
public:
    /**
     * @brief 注册本地工具
     * @param name 工具名称
     * @param func 工具函数
     * @param meta 工具元数据
     */
    void register_local_tool(const std::string& name,
                            std::function<json(const json&)> func,
                            const ToolMeta& meta);
    
    /**
     * @brief 注册 MCP 服务
     * @param service_name 服务名称
     * @param client MCP 客户端
     */
    void register_mcp_service(const std::string& service_name,
                              std::shared_ptr<MCPClient> client);
    
    /**
     * @brief 注册 API 工具
     * @param name 工具名称
     * @param endpoint API 端点
     * @param method HTTP 方法
     * @param meta 工具元数据
     */
    void register_api_tool(const std::string& name,
                          const std::string& endpoint,
                          const std::string& method,
                          const ToolMeta& meta);
    
    /**
     * @brief 统一调用接口
     * @param name 工具名称
     * @param arguments 调用参数（JSON 格式）
     * @return 工具执行结果（JSON 格式，异步 future）
     */
    std::future<json> call_tool(const std::string& name, const json& arguments);
    
    /**
     * @brief 导出工具列表（供 LLM 使用）
     * @return 工具元数据列表
     */
    std::vector<ToolMeta> export_as_llm_tools() const;
    
    /**
     * @brief 查询工具信息
     * @param name 工具名称
     * @return 工具信息（如果存在）
     */
    std::optional<ToolInfo> get_tool_info(const std::string& name) const;
    
    /**
     * @brief 列出所有工具
     * @return 工具名称列表
     */
    std::vector<std::string> list_all_tools() const;
    
private:
    std::map<std::string, std::shared_ptr<ToolInterface>> tools_;
    std::mutex tools_mutex_;
    
    /**
     * @brief 根据工具名称查找工具接口
     * @param name 工具名称
     * @return 工具接口（如果找到）
     */
    std::shared_ptr<ToolInterface> find_tool(const std::string& name) const;
};

} // namespace agent_framework

#endif // __AGENT_TOOLBUS_H__
