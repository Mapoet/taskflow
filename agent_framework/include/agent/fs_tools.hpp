/**
 * @file fs_tools.hpp
 * @brief 内建 fs_* 本地工具注册（需 AGENT_FS_ROOT）
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-03
 */
#ifndef __AGENT_FS_TOOLS_H__
#define __AGENT_FS_TOOLS_H__

#include "toolbus.hpp"

namespace agent_framework {

/**
 * @brief 若 AGENT_FS_ROOT 有效则为 ToolBus 注册 fs_read / fs_write 等；否则 no-op。
 * 已存在同名 fs_read 时跳过（幂等）。
 */
void register_builtin_fs_tools_if_configured(ToolBus& bus);

} // namespace agent_framework

#endif // __AGENT_FS_TOOLS_H__
