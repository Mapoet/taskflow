# §5.1 用户输入 DSL（WP2.7 Tier A）

本文与 [phase-2-wp7.md](./phase-2-wp7.md) v0.2 **扫描顺序与文法**一致；实现见 `UserInputPreprocessor`。

## 扫描顺序（固定）

1. **按行**处理：行首空白去掉后以 `/` 开头的整行视为 `/cmd`，从正文移除并解析（见下节命令白名单）。
2. 在**剩余全文**上从左到右匹配 **`@file(`…`)`**、**`@url(`…`)`**，须**单行内**成对括号；禁止跨行。
3. 去掉已匹配字面后：**折叠**连续空行为至多 **一段 `\n\n`**，**首尾 trim** 得到送 LLM 的 `llm_user_text`。

**已知限制（v1）**：Markdown 代码围栏内的 `/` 打头行**仍**视为 `/cmd`。

## `@file`（v1）

- **形式一（整文件）**：**`@file(`** *path* **`)`** —— *path* 为非空 UTF-8，**不得**含未转义 **`)`**。
- **形式二（行范围）**：**`@file(`** *path* **`,`** *range* **`)`** —— *range* 满足正则 **`^\d+-\d+$`**（闭区间，**从 1 起** 行号），例如 **`12-34`**。
- 物化走 **`ToolBus::call_tool("fs_read", …)`**；路径相对 **`ExecutionContext::cwd`**；须在 **`AGENT_FS_ROOT`** 监禁内（与内建 `fs_read` 一致）。单块正文大小受 **`AGENT_INPUT_FILE_INJECT_MAX_BYTES`**（默认 **262144**）约束。

## `@url`（v1）

- **`@url(`** *url* **`)`**，单行；*`url`* 策略同内建 **`web_fetch`**（HTTPS；HTTP 仅当 **`AGENT_WEB_ALLOW_HTTP=1`**）。
- 物化走 **`call_tool("web_fetch", …)`**。

## `/cmd` 白名单（v1）

| 用户行（trim 后） | 规范化 `command` | `args` |
|-------------------|------------------|--------|
| `/memory compact` | `memory.compact` | `{}` |
| `/memory clear` | `memory.clear` | `{}` |
| `/model <id…>` | `model.set` | `{"id":"<id…>"}`（id 为第二 token 起到行尾 trim，不得为空） |

**未命中**（含 `/mcp`、`/skills` 等）：**不产生** `ControlAction`；严格模式下记 **`command_not_whitelisted:`**；审计日志见实现。

## 注入与 `LLMInput`

- `user_prompt` = `llm_user_text`。
- 物化块按顺序追加到 **`LLMInput.context`**，前缀为：  
  `"\n--- injection:" + source_kind + ":" + source_ref_trunc + "\n"`  
- **`extra_variables["input_policy_version"]`** = `ExecutionContext::input_policy_version`（默认 **`wp27-v1`**）。

## 环境变量（摘录）

| 变量 | 说明 |
|------|------|
| `AGENT_INPUT_STRICT` | 未置或真值 = **严格**；`0` = 宽松（仅排障；生产勿默认） |
| `AGENT_INPUT_TIER_B` | `1` 启用子 LLM 歧义（每输入最多 1 次，5s 超时） |
| `AGENT_BUDGET_MAX_INJECTION_BYTES` | 与 WP2.1c 同源；预处理累计注入+分隔头计字节 |
| `AGENT_FS_ROOT` / `PWD` | `@file` 监禁与 `cwd` |
| `AGENT_INPUT_FILE_INJECT_MAX_BYTES` | 单块文件注入上限（默认 262144） |

## 与后续 WP 的边界

- **`/memory compact`**：本轮产生 `ControlAction`；压缩本体由 **WP2.9** 实现，当前为 **stub**。
- **`/mcp` / 热加载**：**WP3.7**；当前 **拒绝**并审计。
- **Verifier**：**WP2.8**，不在本条目前处理输出。

## CLI / A2A

- **严格**且存在 **`tier_a_violations`**：**CLI** 退出码 **`3`**；**JSON-RPC** **`code = -32602`**，`message` 前缀 **`input_policy_violation`**，`data.violations` 为字符串数组。
- **A2A** 在创建任务前预处理；`task.metadata["wp27_pending"]` 承载注入块与控制动作供 **`wp27_restore_pending_from_task_metadata`** 恢复到 **`AgentThreadState`**。
