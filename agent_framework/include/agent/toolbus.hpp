/**
 * @file toolbus.hpp
 * @brief ToolBus 模块：统一工具管理接口；WP2.1d 调用前 hook 见 `add_tool_call_hook`；WP2.1b 编排 API 在本文件末尾
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
#include <unordered_set>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

#include "mcp_client.hpp"

namespace agent_framework {

namespace detail {
/** @brief 仅单测注入工具以覆盖 tool_not_allowed 路径；勿用于生产 */
struct ToolBusCallHookTestPeer;
}

// ============================================================================
// WP2.1d：调用前 hook（`ToolHookVerdict` 见 types.hpp）
// ============================================================================

/**
 * @brief 单次 hook 回调的返回值
 */
struct ToolHookResult {
    ToolHookVerdict verdict = ToolHookVerdict::Allow;
    /** verdict == Replace 时必填：下一轮 hook 与 schema 校验使用该对象（须为 JSON object） */
    std::optional<json> replaced_arguments;
    /** verdict == Deny 时建议使用非空文案 */
    std::string deny_message;
    /** 并入返回 JSON 的 details（须为 object 时才会合并字段） */
    json deny_details = json::object();
};

/**
 * @brief 调用前 hook：同步、可链式；不得修改 tool_name（由 ToolBus 固定传入）
 */
using ToolCallHook =
    std::function<ToolHookResult(const std::string& tool_name, const json& arguments)>;

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
 * @brief 单个远端 MCP 工具在 ToolBus 中的代理（注册名 service__remote）
 */
class MCPProxyTool : public ToolInterface {
public:
    MCPProxyTool(std::shared_ptr<MCPClient> client, std::string registered_name, std::string remote_tool_name,
                 ToolMeta meta);

    std::future<json> call(const std::string& name, const json& arguments) override;
    ToolMeta get_tool_meta(const std::string& name) const override;
    std::vector<std::string> list_tools() const override;
    bool validate_arguments(const std::string& name, const json& arguments) const override;
    std::optional<ToolInfo> get_tool_info(const std::string& name) const override;

private:
    std::shared_ptr<MCPClient> client_;
    std::string registered_name_;
    std::string remote_tool_name_;
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
    mutable std::mutex cache_mutex_;
    
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
     *
     * 顺序（WP2.1d）：load allowlist → find_tool → is_tool_allowed → **tool call hooks**
     * → validate_tool_arguments → tool->call。详见 docs/guides/tool-call-hooks.md。
     *
     * @param name 工具名称
     * @param arguments 调用参数（JSON 格式）
     * @return 工具执行结果（JSON 格式，异步 future）
     */
    std::future<json> call_tool(const std::string& name, const json& arguments);

    /**
     * @brief 追加调用前 hook（链尾）；空函数抛 std::invalid_argument
     */
    void add_tool_call_hook(ToolCallHook hook);

    /** @brief 清空 hook 链（测试 / demo） */
    void clear_tool_call_hooks();

    /** @brief 当前注册的 hook 数量 */
    std::size_t tool_call_hook_count() const;
    
    /**
     * @brief 导出工具列表（供 LLM 使用）
     * @return 工具元数据列表
     */
    std::vector<ToolMeta> export_as_llm_tools() const;

    /**
     * @brief 工具元数据（含 WP2.1b side_effect）；未知工具返回空 ToolMeta
     */
    ToolMeta get_tool_meta(const std::string& name) const;
    
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

    struct CursorMcpImportFailure {
        std::string service_name;
        std::string reason;
    };

    struct CursorMcpImportResult {
        std::vector<std::string> registered_services;
        std::vector<CursorMcpImportFailure> failures;
    };

    /**
     * @brief 从 Cursor MCP 配置（mcp.json）批量注册 MCP server
     *
     * - 自动识别 server 类型：含 url → HTTP；含 command → stdio
     * - stdio 环境变量策略：继承当前进程环境并叠加 mcp.json 的 env（覆盖同名键）
     *
     * @param config_path 配置文件路径；空表示优先读 env AGENT_MCP_CONFIG_PATH，否则默认 ~/.cursor/mcp.json
     * @param register_all true 表示注册 mcpServers 下所有 server；false 预留（仅一个 server）扩展
     * @return 注册结果（best-effort：失败不会阻断其他 server）
     */
    CursorMcpImportResult register_mcp_from_cursor_config(const std::string& config_path = "",
                                                         bool register_all = true);

private:
    friend struct detail::ToolBusCallHookTestPeer;

    std::map<std::string, std::shared_ptr<ToolInterface>> tools_;
    mutable std::mutex tools_mutex_;
    std::unordered_set<std::string> mcp_services_;
    std::vector<ToolCallHook> hooks_;
    mutable std::mutex hooks_mutex_;
    
    /**
     * @brief 根据工具名称查找工具接口
     * @param name 工具名称
     * @return 工具接口（如果找到）
     */
    std::shared_ptr<ToolInterface> find_tool(const std::string& name) const;
};

// ============================================================================
// 工具编排（WP2.1b，经 ToolBus::call_tool 执行）
// ============================================================================

/**
 * @brief 解析后的编排开关（AgentConfig + 环境变量）
 */
struct ToolOrchestrationOptions {
    bool enable_parallel_reads = false;
    int max_parallel_reads = 4;
};

ToolOrchestrationOptions resolve_tool_orchestration_options(const AgentConfig& cfg);

/**
 * @brief 按工具名解析副作用（通常来自 ToolBus::get_tool_meta(name).side_effect）
 */
using ToolSideEffectResolver = std::function<ToolSideEffect(std::string_view tool_name)>;

/**
 * @brief 读并行（有上限）+ 写/Unknown 串行；结果顺序与 calls 一致
 */
std::vector<json> execute_tool_calls_sequenced(std::shared_ptr<ToolBus> bus,
                                               const std::vector<CallSpec>& calls,
                                               const ToolOrchestrationOptions& opts,
                                               ToolSideEffectResolver classify);

} // namespace agent_framework

#endif // __AGENT_TOOLBUS_H__
