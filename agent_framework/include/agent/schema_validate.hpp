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
 * @brief 根对象上的 JSON Schema 元数据（`$schema` / `$id` / `$comment`）
 *
 * 校验仍按 WP1.2 子集执行；这些键在每一层 schema 中被忽略，仅根级可通过
 * extract_json_schema_root_meta 读出并供日志或后续管线使用。嵌套子 schema 上的同名字段不汇总到此结构。
 */
struct JsonSchemaRootMeta {
    std::optional<std::string> json_schema_uri; ///< `$schema`（非 string 时为 `dump()`）
    std::optional<std::string> id_uri;          ///< `$id`
    std::optional<std::string> comment;         ///< `$comment`（非 string 时为 `dump()`）
};

/**
 * @brief 从根 schema 对象读取元数据字段（不校验、不改写 schema）
 */
void extract_json_schema_root_meta(const json& root_schema, JsonSchemaRootMeta& out);

/**
 * @brief 按 WP1.2 子集校验 arguments 是否满足 schema
 * @param schema OpenAI function parameters 形态（根须 type=object）
 * @param arguments 待校验实例
 * @param error_obj 失败时写入结构化 error、code、details
 * @param root_meta_out 非空则在校验路径开始前写入根级 $schema / $id / $comment（不改变校验结果）
 * @return true 表示校验通过
 *
 * WP1.2 仍不支持 `$ref`、`allOf` 等组合关键字。以下键在任意嵌套层级中被视为元数据并忽略：`$schema`、`$id`、`$comment`。
 * 其余以 `$` 开头的键仍报 schema_unsupported。
 */
bool validate_tool_arguments(const json& schema, const json& arguments, json& error_obj,
                             JsonSchemaRootMeta* root_meta_out = nullptr);

/**
 * @brief Validate an arbitrary JSON instance against the supported JSON Schema subset.
 *
 * Failure details contain both `instance_path` and `schema_path` JSON pointers. This API is
 * used by Skill input/output contracts; `validate_tool_arguments` remains the object-root
 * compatibility wrapper used by ToolBus.
 */
bool validate_json_instance(const json& schema, const json& instance, json& error_obj,
                            JsonSchemaRootMeta* root_meta_out = nullptr);

} // namespace agent_framework

#endif // __AGENT_SCHEMA_VALIDATE_H__
