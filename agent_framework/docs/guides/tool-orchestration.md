# 工具编排（WP2.1b）

本文档说明 **只读并行 + 写串行 + 并发上限** 的语义、配置、与 `ToolBus::call_tool` / `AGENT_TOOL_ALLOWLIST` 的关系，以及内建工具的 `ToolSideEffect` 分类。

## 语义

- **`ToolSideEffect`**：`ReadOnly`（可进入并行读组）、`Write`（始终串行）、`Unknown`（编排上等价 `Write`，串行）。
- **调用顺序**：`CallSpec` 在 LLM 输出中的顺序即权威顺序；`history` 中 `tool` 消息顺序与之一致。并行只改变完成时刻。
- **唯一执行入口**：所有调用仍经 `ToolBus::call_tool`；编排层仅控制何时 `get()` 多个 future，不绕过 allowlist。

## 配置（优先级从高到低）

| 来源 | 作用 |
|------|------|
| 环境变量 `AGENT_TOOL_PARALLEL_READS` | 若设为 `0`/`false`/`off`/`no`：强制关闭并行读；`1`/`true`/`on`/`yes`：强制开启（覆盖 `AgentConfig`） |
| 环境变量 `AGENT_TOOL_MAX_PARALLEL` | 合法正整数时覆盖 `max_parallel_read_tools` |
| `AgentConfig::enable_parallel_read_tools` | 默认 `false` |
| `AgentConfig::max_parallel_read_tools` | 默认 `4`；`<=0` 在 `resolve_tool_orchestration_options` 中视为 `1` |

解析函数：`resolve_tool_orchestration_options(const AgentConfig&)`（声明于 `toolbus.hpp`）。

## 与 `AGENT_TOOL_ALLOWLIST`

allowlist 在 **`call_tool`** 时生效；并行仅同时发起多个**已允许**工具的 `call_tool`。请勿绕过 allowlist 注册或调用。

## Agent 循环与 repeat guard

在 **任意** `call_tool` 之前，对每个 `CallSpec` 按原顺序做 `guard_repeat_tool_in_iteration` 检测；若在并行读组内某索引触发 guard，**该组及之后**调用均不执行（与历史 `break` 一致）。实现见 `agent_loop_node.cpp` 工具阶段。

## 内建工具 `side_effect` 表（与代码一致）

| 工具名 | `ToolSideEffect` | 备注 |
|--------|------------------|------|
| `fs_read`, `fs_list_dir`, `fs_search`, `fs_grep` | `ReadOnly` | 沙箱内只读 |
| `fs_write`, `fs_mkdir`, `fs_delete`, `fs_replace` | `Write` | |
| `web_search`, `web_fetch`, `web_rss_feed`, `web_configured_source` | `ReadOnly` | |
| `web_fetch_archive` | `Write` | 解压写入 `AGENT_FS_ROOT` 下 |
| `expr_eval`, `expr_validate`, `expr_batch_eval` | `ReadOnly` | |
| `draw_render` | `ReadOnly` | 返回 base64 |
| `draw_export` | `Write` | 写盘 |
| `run_skill_script` | `Write` | 子进程 |

**MCP 工具**：列表元数据默认 `Unknown` → 串行。若将来在导入层映射 `side_effect`，再更新本节。

## 工具作者责任

- 若工具依赖进程内可变状态且**非线程安全**，须标为 `Unknown` 或 `Write`，或自行加锁。
- 误标 `ReadOnly` 可能导致并行下的数据竞争；编排层不验证实现是否只读。

## API

- `execute_tool_calls_sequenced` / `ToolOrchestrationOptions` / `ToolSideEffectResolver`：`toolbus.hpp`
- `ToolSideEffect` / `tool_side_effect_from_string`：`types.hpp`
- `ToolCallNode::create_parallel(..., const ToolOrchestrationOptions& orch_opts = {})`：默认关闭并行读，与旧版「无上限 async」相比更安全；需要并行时传入 `enable_parallel_reads = true` 的选项（并可配合 env 覆盖）。

**文档版本**：0.1  
**日期**：2026-04-05
