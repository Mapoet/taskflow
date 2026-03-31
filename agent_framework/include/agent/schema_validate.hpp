/**
 * @file schema_validate.hpp
 * @brief JSON Schema 子集校验（工具参数），供 LocalTool / ToolBus 共用
 * @author Mapoet
 * @version 0.1
 * @date 2026-03-31
 */
#ifndef __AGENT_SCHEMA_VALIDATE_H__
#define __AGENT_SCHEMA_VALIDATE_H__

#include "types.hpp"

namespace agent_framework {

/**
 * @brief 按 WP1.2 子集校验 arguments 是否满足 schema
 * @param schema OpenAI function parameters 形态（根须 type=object）
 * @param arguments 待校验实例
 * @param error_obj 失败时写入 §5 形状：error、code、details
 * @return true 表示校验通过
 */
bool validate_tool_arguments(const json& schema, const json& arguments, json& error_obj);

} // namespace agent_framework

#endif // __AGENT_SCHEMA_VALIDATE_H__
