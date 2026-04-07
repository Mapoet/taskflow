# WP2.8 Verifier（第二套 LLM）

主 Agent 图产出 **`final_answer` 草案** 后，可由 **Verifier**（框架内 **第二套 `LLMClient`**，`LLMInput.tools` **为空**）输出结构化 JSON，再经 **闸逻辑** 决定是否发布、整图重跑或失败退出。规范全文见 [phase-2-wp8.md](./phase-2-wp8.md)。

## 用户可见行为

| 场景 | CLI | A2A |
|------|-----|-----|
| **Pass** | 终稿即 Verifier 认可的答案；Sink JSON 含 `verifier_ok: true` | 正常 **COMPLETED** |
| **Pass-through** | 仍为草案文本；`verifier_ok: false`，`issues` 可非空 | 仍可 **COMPLETED**；载荷见 `suggested_action` / `issues` |
| **Retry MAIN** | 同一次用户请求内、在 **次数上限** 内追加 **system hint** 后重跑主图 | 与 CLI 共用 `GraphExecutor::run_react_cli_sync` 时行为一致 |
| **Abort** | **stderr** 摘要 + **退出码 `4`**（与 WP2.7 输入质控 **`3`** 区分） | **FAILED**；错误信息含 **`verifier_abort`**；SSE 可走失败通道 |

## 环境变量（摘要）

| 变量 | 默认 | 说明 |
|------|------|------|
| `AGENT_VERIFIER` | `off` | `off`：零额外延迟；`on`：每轮终稿后必跑 Verifier；`sample`：按 `AGENT_VERIFIER_SAMPLE_RATE`（0–1，默认 `0.1`）抽样 |
| `AGENT_VERIFIER_MAX_RETRIES` | `1` | Verifier 触发的 **额外 MAIN** 次数上限 |
| `AGENT_VERIFIER_TIMEOUT_MS` | `30000` | 超时 → 合成 `pass_through`（`verifier_timeout`），**不** `retry_main` |
| `AGENT_VERIFIER_MODEL` | 空 | 空则继承主链路 `AGENT_LLM_*`；非空则为 Verifier 专用模型名 |
| `AGENT_VERIFIER_TEMPERATURE` | `0` | |
| `AGENT_VERIFIER_PROMPT_MAX_BYTES` | `65536` | 用户 JSON 总长上限；超长截断 `draft_final_answer` 尾部 |
| `AGENT_VERIFIER_INCLUDE_TOOL_TRACE` | `0` | |
| `AGENT_VERIFIER_REDACT_IDS` | `0` | `1` 时不把 `session_id`/`task_id` 拼进 Verifier 用户 JSON |
| `AGENT_VERIFIER_HISTORY_TURNS` | `3` | `history_digest` 摘要轮数 |

## Sink JSON 扩展字段（终稿 Sink）

除原有 `final_answer`、`iteration`、`history_size` 等外，Verifier 开启且已运行时可能包含：

- `verifier_ok`（boolean）
- `suggested_action`（`pass` / `retry_main` / `pass_through` / `abort` 的 **闸后** 有效值）
- `issues`（数组，元素 `code` / `severity` / `detail`）

## 会话合并与 Retry（MAIN 第二趟）

- **首次** MAIN 成功退出后：`merge_react_session_state(..., FullUserTurn)` → `history = old + user(本轮快照) + delta`。
- **Verifier `retry_main`** 且未超上限：向 `session.history` 追加一条 **`role=system`** 的 FIX 提示（[phase-2-wp8.md §5.3](./phase-2-wp8.md)），`verifier_retry_count++`，恢复 `session.initial_user_prompt` 为本轮进入 `run_react_cli_sync` 时的用户快照后 **再跑 MAIN**。
- **第二趟及以后** MAIN 合并：`merge_react_session_state(..., DeltaOnly)` → **不** 再插入第二条 user，避免与 WP2.0 合并语义冲突。

## SSE（A2A）

- `AgentServer::push_verifier_sse(task_id, "verifier_started", payload)` / `"verifier_completed"`：载荷带 `component=verifier` 及 `task_id`、`session_id`、`ts` 等。
- 业务路径也可设置 `ReactCliRunOptions::on_verifier_event`，由 `task_handler` 转调 `push_verifier_sse` 以与 CLI **同源** 编排对齐。

## 单测

- 闸与解析：`ctest -R verifier_types`
- Runner mock：`ctest -R verifier_runner`
- 图编排与事件钩子：`ctest -R verifier_graph_hooks_i1`

## 与 WP2.9 边界

Verifier **不做** 长期记忆压缩；**不** 接外部工单平台；**不** 用专用 RAG；**不** 在 FIX 中附全文 `draft_final_answer`（仅 issues JSON 摘要）。
