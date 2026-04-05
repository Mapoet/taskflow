# 上下文预算（WP2.1c）

本文描述工具结果与用户注入（`context` / `skill_block` / `user_prompt`）在送入 LLM 前的三层字节上限、截断形态与环境变量。实现见 `include/agent/context_budget.hpp`（`AfTruncationKind` 在 `types.hpp`）与 `src/context_budget/*.cpp`。

## 方案 S / T

- **v1 仅采用方案 S**：仅当紧凑 `dump()` 的 UTF-8 字节数超过对应上限时才替换为带 `_af_truncation` 的包装 JSON；未超限时原样保留（小结果无 `_af_truncation`）。
- **未采用方案 T**（全量摘要替换）。

## 环境变量与 `AgentConfig.extra_config`

优先级：`extra_config` 键覆盖环境变量；键名与 env 同名去掉前缀 `AGENT_`（如 `BUDGET_MAX_TOOL_RESULT_JSON_BYTES`）。

| 变量 | 默认 | 含义 |
|------|------|------|
| `AGENT_BUDGET_MAX_TOOL_RESULT_JSON_BYTES` | 1048576 | 单工具 `tool_result` 紧凑 `dump()` 上限 |
| `AGENT_BUDGET_MAX_INJECTION_BYTES` | 2097152 | `context`+`skill_block`+`user_prompt`（模板后）合计 UTF-8 字节 |
| `AGENT_BUDGET_MAX_COMBINED_PROMPT_ATTACH_BYTES` | 8388608 | 所有 tool 的 `tool_result` `dump()` 之和加上述三段文本 |
| `AGENT_BUDGET_MAX_RENDERED_MESSAGES_BYTES` | 0 | `rendered.messages` 各条 `dump()` 总字节上限；可与 `set_max_tokens` 的 `tokens×4` 取小 |
| `AGENT_CONTEXT_BUDGET_SPILL_DIR` | 空 | 可写则 spill 完整 dump；`ref` 为文件名，`spill: true` |
| `AGENT_CONTEXT_BUDGET_STRICT` | 0 | `1` 时仍超限则 `RenderedPrompt::context_budget_blocked`，跳过 LLM 调用 |
| `AGENT_BUDGET_MAX_WIRE_MESSAGE_BYTES` | 4194304 | 可选线路帽；`apply_wire_payload_cap` |

非法无符号整数：警告 + 默认值（不抛）。

## `_af_truncation` schema

- `_af_truncation`: `kind`, `original_utf8_bytes`, `kept_utf8_bytes`, `spill`, `ref`, 可选 `reason`
- `preview_text`：合并帽占位须含字面 `combined_budget_exhausted`

## 调用点

1. 单工具帽：`agent_loop_node.cpp` 的 `append_tool_message` 与 `tool_agg`。
2. 注入帽：`prompt_renderer.cpp` 内 `apply_injection_cap`；超限追加 `## af_budget` + JSON。
3. 合并帽：`render()` 内在 `truncate` 之后、`format_as_messages` 之前对 history **副本** `apply_combined_to_history_slice`（最早 tool 先降级）。
4. 渲染总字节帽：仅 `truncate_prompt`。

## 测试

- `context_budget_wp21c`：`tests/test_context_budget.cpp`
- `agent_loop_context_budget_i1_wp21c`：`tests/test_agent_loop_context_budget_wp21c.cpp`

## 相关

- [phase-2-wp1c.md](./phase-2-wp1c.md)
- [phase-2-plan.md](./phase-2-plan.md) D7
