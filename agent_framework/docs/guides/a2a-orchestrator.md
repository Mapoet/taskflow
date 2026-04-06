# A2A 编排器（peers.json + `cli_a2a_orchestrator_demo`）

本指南描述 **本地 ReAct 进程**如何通过 ToolBus 调用多个远端 A2A Agent（JSON-RPC 默认路径）。基线见 **[phase-2-wp-agent2agent.md](./phase-2-wp-agent2agent.md)**；**细粒度多子任务编排、并行 submit、digest 注入** 见 **[plan-detailed-multi-agents.md](./plan-detailed-multi-agents.md)**（WP2.agents）。

---

## 1. 配置文件

仓库内样例（可直接作为 `--peers-file`）：

| 文件 | 说明 |
|------|------|
| `agent_framework/examples/configs/a2a_peers.min.json` | **最小**：单 peer `local` → `http://127.0.0.1:9001` |
| `agent_framework/examples/configs/a2a_peers.example.json` | 双 peer（worker + reviewer），含 `auth.token_env` 示例 |

复制到自定义路径后按需改 `origin` / 端口。字段说明见详案 §3。`auth.token_env` 表示编排进程启动时从**环境变量**读 token 并注入 Bearer（**勿**把 secret 写入仓库）。

---

## 2. 构建与运行

在启用示例的前提下构建目标 **`cli_a2a_orchestrator_demo`**（与 `cli_agent_skills_demo` 相同 CLI11 依赖）。

在**仓库根**下 `build/` 里可执行文件一般在 `build/agent_framework/cli_a2a_orchestrator_demo`，样例 peers 用源码树相对路径即可：

```bash
cd build
./agent_framework/cli_a2a_orchestrator_demo \
  --peers-file ../agent_framework/examples/configs/a2a_peers.min.json -v
./agent_framework/cli_a2a_orchestrator_demo \
  --peers-file ../agent_framework/examples/configs/a2a_peers.min.json \
  -p "请通过 a2a_send_message 向 local 发一句问候"
```

任意路径：

```bash
./cli_a2a_orchestrator_demo --peers-file /path/to/peers.json -v
./cli_a2a_orchestrator_demo --peers-file /path/to/peers.json -p "Ask worker to summarize X"
```

常用选项：

| 选项 | 含义 |
|------|------|
| `--peers-file` | `peers.json` 路径（必填） |
| `-p` / `--prompt` | 单轮用户消息后退出 |
| `-v` / `--verbose` | `AGENT_LOG_LEVEL=debug` |
| `--provider` | 覆盖 `AGENT_LLM_PROVIDER` |
| `--max-iterations` | 覆盖 `AgentConfig::max_iterations` |
| `--merge-streams` | 将远端状态行并入与本地 LLM 相同的流式 handler |
| `--register-peer-aliases` | 额外注册 `a2a_send_message__<peer_id>`（省略 `peer_id` 参数） |

启用细粒度工具时，`AGENT_TOOL_ALLOWLIST` 还须包含 `a2a_submit_task`、`a2a_wait_tasks`、`a2a_get_task_status` 等（见 §4.2）；或留空关闭限制。

---

## 3. 与两个 `agent_server_demo` 联调（Tier D 风格）

**终端 A**（worker，示例端口 9001）：

```bash
export AGENT_SERVER_CARD_PUBLIC_BASE=http://127.0.0.1:9001
export AGENT_SERVER_JSON_RPC_PATH=/rpc
export AGENT_SERVER_LEGACY_REST=0
export AGENT_SERVER_DEMO_ROLE=integration-worker
# 若启用鉴权：与 peers.json / AGENT_A2A_LIVE_TOKEN 对齐
./agent_server_demo --port 9001
```

**终端 B**（reviewer，端口 9002）：同上，改 `AGENT_SERVER_CARD_PUBLIC_BASE`、端口与 `AGENT_SERVER_DEMO_ROLE=integration-reviewer`（或你的第二角色）。

**编排机**：

1. 编写 `peers.json`，`origin` 指向上述两个基 URL（勿尾斜杠混用 discover 亦可，库会规整）。  
2. 若使用 `token_env`，导出对应变量（如 `AGENT_A2A_LIVE_TOKEN`）。  
3. 设置 **`AGENT_TOOL_ALLOWLIST`** 包含 `a2a.send_message`（及若启用别名的 `a2a.send_message__worker` 等），或留空以关闭限制（见 [envs_status.md](./envs_status.md)）。  
4. 运行 `cli_a2a_orchestrator_demo --peers-file ...`。

---

## 4. 工具契约

### 4.1 简易模式：`a2a_send_message`

- **名**：`a2a_send_message`（`kA2aOrchestratorToolSendMessage`）  
- **参数**：`peer_id`、`user_text`（必填）；`continue_session`（默认 `true`）；`metadata`（可选 object）  
- **返回**：`ok`、`peer_id`、`task_id`、`status`、`summary_text`、`context_id`、`error`、`a2a_error_code`（RPC 错误时）  
- **`ToolMeta::side_effect`**：**Write**（同步等待远端到终态的「一把梭」路径）。

### 4.2 细粒度模式（`OutboundTaskSupervisor`，WP2.agents）

实现：`register_a2a_orchestrator_tools(..., std::shared_ptr<OutboundTaskSupervisor>, ...)` 在保留 `a2a_send_message` 的同时注册下表工具。字段与 JSON 形状以 **[plan-detailed-multi-agents.md](./plan-detailed-multi-agents.md) §5** 为准。

| 工具名 | `ToolSideEffect` | 说明 |
|--------|------------------|------|
| `a2a_submit_task` | Write | `SendMessage` → 分配 `local_handle`，可选后台监测（SSE/轮询与 `run_remote_task_and_wait` 同源辅助） |
| `a2a_wait_tasks` | Write | 等待一组 `local_handle` 到终态 |
| `a2a_cancel_task` | Write | 本地停止监测 + 尽力远端 `CancelTask`；`remote_ack` 可能为 `false` |
| `a2a_extend_task_timeout` | Write | 非终态任务延长本地 deadline |
| `a2a_get_task_status` | ReadOnly | 只读快照 |
| `a2a_list_subtasks` | ReadOnly | 子任务事件环 + `next_seq`（通道 C） |

**并发**：同轮 **连续** 的多个 `a2a_submit_task` 可按 `AgentConfig::max_parallel_a2a_submits`（默认 `4`，`AGENT_A2A_MAX_PARALLEL_SUBMITS` 可覆盖）并行 `call_tool`；其它 **Write** 仍串行；**ReadOnly** 仍走 WP2.1b 并行读。详见 [tool-orchestration.md](./tool-orchestration.md) 附录。

**可观测性**：通道 A — `AGENT_A2A_SUBTASK_LOG`（`stderr` / `none`）；通道 B — `LLMInput::orchestrator_subtask_digest` 经 `PromptRenderer::render` 注入 system 块标题 `## Outbound subtasks (auto)`；通道 C — `a2a_list_subtasks`。

**新用户轮**：`cli_a2a_orchestrator_demo` 每行用户输入开始可调用 `OutboundTaskSupervisor::on_user_turn_barrier()`；若进程启动时 **`AGENT_A2A_CANCEL_ON_NEW_TURN=1`**（policy 与此一致）则取消仍在活动的出站任务。

---

## 5. 环境变量（WP2.agents 摘要）

完整表见 [envs_status.md](./envs_status.md) §3.13：`AGENT_A2A_MAX_PARALLEL_SUBMITS`、`AGENT_A2A_CANCEL_ON_NEW_TURN`、`AGENT_A2A_SUBTASK_LOG`、`AGENT_A2A_SUBTASK_CONTEXT_MAX_EVENTS`、`AGENT_A2A_SUBTASK_CONTEXT_BYTES`。

---

## 6. 相关链接

- [plan-detailed-multi-agents.md](./plan-detailed-multi-agents.md)  
- [a2a-spec-tracker.md](./a2a-spec-tracker.md)  
- [agent-client.md](./agent-client.md)  
- [a2a-integration-tests.md](./a2a-integration-tests.md)  
- [context-budget.md](./context-budget.md)  
- [tool-orchestration.md](./tool-orchestration.md)  
