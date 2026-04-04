/**
 * @file draw_tools.hpp
 * @brief 内建 draw_render / draw_export（canvas_ity + stb；AGENT_DRAW_ENABLE）
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_DRAW_TOOLS_H__
#define __AGENT_DRAW_TOOLS_H__

#include "toolbus.hpp"

namespace agent_framework {

/**
 * @brief 当 AGENT_DRAW_ENABLE 未关闭且编译启用 canvas_ity + stb_image_write 时注册 draw_* 工具。
 * 幂等：已存在 draw_render 则跳过。
 */
void register_builtin_draw_tools_if_configured(ToolBus& bus);

} // namespace agent_framework

#endif // __AGENT_DRAW_TOOLS_H__
