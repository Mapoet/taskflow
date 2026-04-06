/**
 * @file execution_context.hpp
 * @brief WP2.7：ExecutionContext（cwd、策略版本、会话/任务 id）
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_EXECUTION_CONTEXT_H__
#define __AGENT_EXECUTION_CONTEXT_H__

#include <optional>
#include <string>
#include <unordered_set>

namespace agent_framework {

/**
 * @brief 单次执行输入策略上下文；在拼装 LLMInput 前固化
 */
struct ExecutionContext {
    /** @brief 解析相对 @file 的基准路径 */
    std::string cwd;
    /** @brief 空 = 不限制；非空时 Tier A 可对 MCP 依赖扩展预留 */
    std::unordered_set<std::string> allowed_mcp_services;
    /** @brief 写入 LLMInput.extra_variables 与审计日志 */
    std::string input_policy_version = "wp27-v1";
    std::optional<std::string> session_id;
    std::optional<std::string> task_id;

    /**
     * @brief 从环境构造：AGENT_FS_ROOT（经 fs 模块）、PWD、当前路径
     */
    static ExecutionContext from_environment();
};

} // namespace agent_framework

#endif // __AGENT_EXECUTION_CONTEXT_H__
