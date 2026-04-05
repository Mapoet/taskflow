# A2A 编排器（peers.json + `cli_a2a_orchestrator_demo`）

本指南描述 **本地 ReAct 进程**如何通过 ToolBus 工具 **`a2a.send_message`** 调用多个远端 A2A Agent（JSON-RPC 默认路径）。详案见 **[phase-2-wp-agent2agent.md](./phase-2-wp-agent2agent.md)**。

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
  -p "请通过 a2a.send_message 向 local 发一句问候"
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
| `--register-peer-aliases` | 额外注册 `a2a.send_message__<peer_id>`（省略 `peer_id` 参数） |

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

- **名**：`a2a.send_message`  
- **参数**：`peer_id`、`user_text`（必填）；`continue_session`（默认 `true`）；`metadata`（可选 object）  
- **返回**：`ok`、`peer_id`、`task_id`、`status`、`summary_text`、`context_id`、`error`、`a2a_error_code`（RPC 错误时）

`ToolMeta::side_effect` 为 **Write**（与远端有状态任务语义一致）。只读探测类工具若未来增加，可标 **ReadOnly**（见 [tool-orchestration.md](./tool-orchestration.md)）。

---

## 5. 相关链接

- [a2a-spec-tracker.md](./a2a-spec-tracker.md)  
- [agent-client.md](./agent-client.md)  
- [a2a-integration-tests.md](./a2a-integration-tests.md)  
- [context-budget.md](./context-budget.md)  
