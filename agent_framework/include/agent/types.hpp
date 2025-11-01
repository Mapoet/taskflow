/**
 * @file types.hpp
 * @brief 公共数据结构定义
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_TYPES_H__
#define __AGENT_TYPES_H__

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>

using json = nlohmann::json;

namespace agent {

// 工具元数据
struct ToolMeta {
    std::string name;              // 工具名称
    json schema;                  // JSON Schema 描述（OpenAI Function Calling 格式）
    std::string description;       // 工具说明
};

// LLM 输入结构
struct LLMInput {
    std::string system_prompt;     // 系统提示词（角色定义、行为规范）
    std::string user_prompt;       // 用户提示词（当前问题或指令）
    std::string context;           // 从知识库检索的内容摘要（多模态 RAG 结果）
    std::vector<ToolMeta> tools;   // 可用工具列表（ToolBus 导出）
    std::optional<std::string> image_data;  // 图像 base64 编码（可选）
    std::optional<std::string> audio_data;   // 音频 base64 编码（可选）
};

// 工具调用规范
struct CallSpec {
    std::string name;              // 工具名称
    json arguments;                // 调用参数（JSON 对象）
};

// LLM 输出结构
struct LLMOutput {
    std::vector<CallSpec> tool_calls;  // 工具调用列表
    std::string reasoning;             // 中间思考和计划描述
    bool is_final;                     // 是否已完成任务（true 表示生成最终答案）
    std::string final_answer;          // 最终回答（仅当 is_final 为真时有效）
    std::optional<std::string> audio_out;  // 语音输出（可选，如 GPT-4o Realtime API）
    std::optional<std::string> image_out;  // 图像输出（可选，如 GPT-4o Realtime API）
};

} // namespace agent

#endif // __AGENT_TYPES_H__

