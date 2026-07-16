/**
 * @file expr_tools.hpp
 * @brief 内建 expr_eval / expr_validate / expr_batch_eval（ExprTk；AGENT_EXPR_ENABLE）
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-04
 */
#ifndef __AGENT_EXPR_TOOLS_H__
#define __AGENT_EXPR_TOOLS_H__

#include <agent/toolbus/toolbus.hpp>

namespace agent_framework {

/**
 * @brief 当 AGENT_EXPR_ENABLE 未关闭且编译启用 ExprTk 时注册 expr_* 工具。
 * 幂等：已存在 expr_eval 则跳过。
 */
void register_builtin_expr_tools_if_configured(ToolBus& bus);

} // namespace agent_framework

#endif // __AGENT_EXPR_TOOLS_H__
