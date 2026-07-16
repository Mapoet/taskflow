/**
 * @file context_budget.hpp
 * @brief WP2.1c：工具结果与用户注入上下文预算（单工具帽、合并帽、注入帽、方案 S）
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_CONTEXT_BUDGET_H__
#define __AGENT_CONTEXT_BUDGET_H__

#include <agent/core/types.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace agent_framework {

/**
 * @brief 方案 S 元数据（序列化进 `_af_truncation` 对象）
 */
struct AfTruncationMeta {
    AfTruncationKind kind = AfTruncationKind::tool_result;
    std::size_t original_utf8_bytes = 0;
    std::size_t kept_utf8_bytes = 0;
    bool spill = false;
    std::string ref;
    /** 可选：合并帽降级等 */
    std::string reason;
};

/**
 * @brief 环境 / extra_config 解析后的预算上限（非法值回默认并打日志）
 */
struct ContextBudgetLimits {
    std::size_t max_tool_result_json_bytes = 1048576;
    std::size_t max_injection_bytes = 2097152;
    std::size_t max_combined_prompt_attach_bytes = 8388608;
    /** 0 = 不限制 `rendered.messages` 总 dump 字节 */
    std::size_t max_rendered_messages_bytes = 0;
    std::size_t max_wire_message_bytes = 4194304;
    std::string spill_dir;
    bool context_budget_strict = false;

    /**
     * @brief 从环境变量加载；`agent_cfg` 非空时 `extra_config` 键名与 env 同名去 `AGENT_` 前缀覆盖 env
     */
    static ContextBudgetLimits load(const AgentConfig* agent_cfg = nullptr);
};

const char* af_truncation_kind_string(AfTruncationKind kind);

/** 紧凑 UTF-8 dump 字节数；失败时返回占位串长度 */
std::size_t json_utf8_dump_bytes(const nlohmann::json& j);

/** 紧凑 dump（非法 UTF-8 可能抛 — 由调用方 try/catch；内部工具函数） */
std::string json_compact_dump(const nlohmann::json& j);

/**
 * @brief UTF-8 安全前缀截断（不在码点中间切开）
 */
std::string utf8_safe_truncate(std::string_view utf8_text, std::size_t max_bytes);

nlohmann::json wrap_truncated_payload(const AfTruncationMeta& meta, std::string_view preview_utf8);

/**
 * @brief 单工具结果帽（方案 S）：`dump(in_out) <= max` 则不变；否则原地替换为 `_af_truncation` + `preview_text`
 * @return 是否仍满足上限（正常为 true）；`warn_out` 可填解析告警
 */
bool apply_per_tool_result_budget(nlohmann::json& in_out, const ContextBudgetLimits& limits,
                                  AfTruncationKind kind, std::string* warn_out = nullptr);

/** 合并帽占位：含字面 `combined_budget_exhausted` */
nlohmann::json combined_budget_exhausted_placeholder(std::size_t original_dump_bytes);

/**
 * @brief spill 完整 dump；失败静默 `spill=false`
 */
void apply_spill_if_configured(const ContextBudgetLimits& limits, std::string_view full_dump_utf8,
                               AfTruncationMeta& meta_in_out);

/**
 * @brief A2A / 线路上限（可选）：超限则替换为方案 S 包裹体
 */
bool apply_wire_payload_cap(nlohmann::json& in_out, std::size_t max_bytes,
                            std::string* warn_out = nullptr);

bool is_combined_budget_stub(const nlohmann::json& tool_result);

/**
 * @brief 合并帽：对 `history` 副本从**最早** tool 消息起替换为 `combined_budget_exhausted` 占位，直到
 *        `tool_result` dump 之和 + 文本字段 UTF-8 字节 ≤ `max_combined_prompt_attach_bytes`
 */
class ContextBudgetMeter {
public:
    void reset_for_turn() {}

    static void apply_combined_to_history_slice(std::vector<Message>& history_for_render,
                                                const std::string& user_prompt_utf8,
                                                const std::string& context_utf8,
                                                const std::string& skill_block_utf8,
                                                const ContextBudgetLimits& limits);

    /** 供 STRICT 判定：`tool` 序列化字节 + 三段文本 UTF-8 */
    static std::size_t combined_attach_bytes(const std::vector<Message>& history,
                                             const std::string& user_prompt_utf8,
                                             const std::string& context_utf8,
                                             const std::string& skill_block_utf8);
};

/**
 * @brief 注入帽：`context` + `skill_block` + `user_prompt` 总 UTF-8 超限则从合并串**前缀保留**侧截断，
 *        并生成 `out_af_budget_section`（`## af_budget` + 单行 JSON，可为空）
 */
void apply_injection_cap(std::string& context_utf8, std::string& skill_block_utf8,
                         std::string& user_prompt_utf8, const ContextBudgetLimits& limits,
                         std::string& out_af_budget_section);

} // namespace agent_framework

#endif // __AGENT_CONTEXT_BUDGET_H__
