/**
 * @file skill_script_tool.hpp
 * @brief WP1.8 L3：向 ToolBus 注册 `run_skill_script`
 */
#ifndef __AGENT_SKILL_SCRIPT_TOOL_H__
#define __AGENT_SKILL_SCRIPT_TOOL_H__

#include <agent/skills/skill_services.hpp>
#include <agent/toolbus/toolbus.hpp>

namespace agent_framework {

/**
 * 在 `services` 非空时注册 `run_skill_script`。
 * **幂等**：同一 `ToolBus` 上已注册 `run_skill_script` 时直接返回（便于 REPL 每行重建图）。
 */
void register_skill_script_tool(ToolBus& bus, const std::shared_ptr<SkillServices>& services);

} // namespace agent_framework

#endif // __AGENT_SKILL_SCRIPT_TOOL_H__
