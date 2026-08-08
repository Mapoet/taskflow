# AgentServer（WP2.2 + WP2.3）

本文档描述 `AgentServer` 的监听方式、环境变量、与 [a2a-spec-tracker.md](./a2a-spec-tracker.md) 对齐的 HTTP 面、队列背压，以及 **WP2.3** 任务状态机、取消与超时。

## 监听与端口

- **`AgentServer::start()`**：阻塞调用，内部使用 `httplib::Server::listen`（或 `bind_to_any_port` + `listen_after_bind`）。
- 构造函数传入 **`port == 0`** 时，使用临时端口；成功后可通过 **`bound_port()`** 读取实际端口。
- **`AGENT_SERVER_BIND`**：绑定地址，默认 `0.0.0.0`。
- **`AGENT_SERVER_PORT`**：若设置，覆盖构造函数传入的端口（0–65535）。

调用方宜在独立线程中运行 `start()`，在另一线程调用 `stop()` 以结束监听。

## 路由与规范对齐

| 路由 | 说明 |
|------|------|
| `GET /.well-known/agent-card.json` | Well-Known Agent Card，正文为 WP2.1a **wire**（`agent_card_discovery_json_string`） |
| `POST {jsonrpc_path}` | 单一路径 JSON-RPC 2.0；`Content-Type: application/json` |
| `SendStreamingMessage` | JSON-RPC POST 后切换为 chunked SSE，并创建任务 |
| `SubscribeToTask` | JSON-RPC POST 后切换为 chunked SSE，先重放当前快照 |
| `GET /tasks/sendSubscribe?task_id=` | 兼容订阅端点；非 legacy Client 不再使用 |
| `GET /health` | **非规范**诊断端点，`200` + `{"ok":true}` |

### `jsonrpc_path` 解析

1. 若 **`AGENT_SERVER_JSON_RPC_PATH`** 非空，以其为准（自动补前导 `/`）。
2. 否则从 **`AgentCard::api_endpoint`** 解析：以 `http://` / `https://` 开头时取第一个路径段（含 `/`）；若以 `/` 开头则整串作为路径；否则回退 **`/`**。

### JSON-RPC 与 HTTP 状态码

- 所有 JSON-RPC 响应（含 `error`）使用 **HTTP 200** 与 `application/json` 正文。
- 认证失败时 JSON-RPC 入口可返回 **HTTP 401**。

### 背压

- 任务队列满时：`SendMessage` 返回 JSON-RPC **`error.code = -32001`**，`message` 为 **`queue_full`**，可选 `data.max_queued`。
- Legacy `POST /tasks/send` 在队列满时返回 **HTTP 503**。

## 环境变量一览

| 变量 | 默认 | 说明 |
|------|------|------|
| `AGENT_SERVER_BIND` | `0.0.0.0` | 监听地址 |
| `AGENT_SERVER_PORT` | 构造参数 | 覆盖端口 |
| `AGENT_SERVER_JSON_RPC_PATH` | 从 Card 解析 | 覆盖 JSON-RPC POST 路径 |
| `AGENT_A2A_STRICT` | `1` | 为 `1` 时，Legacy 仅在与 `AGENT_SERVER_LEGACY_REST` 组合且本变量为 `0` 时启用（见实现） |
| `AGENT_SERVER_LEGACY_REST` | `0` | `1` 且 **`AGENT_A2A_STRICT=0`** 时注册 `/tasks/send` 等过渡路由 |
| `AGENT_SERVER_MAX_QUEUED_TASKS` | `64` | 有界队列长度 |
| `AGENT_SERVER_WORKER_THREADS` | `max(2, hw/2)` | Worker 线程数 |
| `AGENT_SERVER_EXECUTOR_THREADS` | `hardware_concurrency`（裁剪） | 统一 GraphExecutor 使用的 `tf::Executor` |
| `AGENT_SESSION_DB` | `.agent/session.db` | Server 默认 SQLite SessionStore 路径 |
| `AGENT_SERVER_SSE_PING_SEC` | `30` | SSE 注释帧间隔；`0` 禁用 |
| `AGENT_SERVER_SSE_QUEUE_CAP` | `256` | 每个 SSE 订阅者的最大待发送帧数；满时丢弃最旧帧 |
| `AGENT_SERVER_SSE_MAX_DROPPED` | `1024` | 单订阅累计丢弃达到该值后关闭慢订阅 |
| `AGENT_TASK_DEFAULT_TIMEOUT_SEC` | `0` | 默认 wall-clock 超时（秒）；`0` 表示无默认超时 |
| `AGENT_TASK_MAX_TIMEOUT_SEC` | `86400` | 单任务超时上限（含 `metadata.timeout_sec`），超出则钳制并告警 |

### Execution profile 与 `TaskControl`

Server 启动前必须配置 `AgentExecutionProfile`。每个任务由 Server 构造
`ExecutionRequest` 并调用 `GraphExecutor::execute_sync`；旧 `set_task_handler` 双轨接口已删除。
未显式注入 SessionStore 时，Server 使用 SQLite；测试可注入 `InMemorySessionStore`。

## Live demo 与启动器

`agent_server_demo` 是**仅 Live** 的对外服务示例：它与 `cli_agent_demo`、TUI、Web、ImGui
共享 `examples/common/agent_example_bootstrap.hpp::build_live_runtime`，统一初始化真实 LLM、
Cursor MCP、Skills、FS/WEB/Expr/Draw 和提示词。它不提供 mock 角色；确定性 A2A 覆盖应使用进程内
`AgentServer` + fake adapter 测试。

```bash
cp agent_framework/examples/configs/server.env.example .env.server
agent_framework/tools/run_agent_server.sh --port 9001 --fs-root "$PWD"
```

启动器优先读取 `.env.server`，然后复用 `.env.ui` 的 LLM/MCP/Skills 键；Live 默认启用
`AGENT_VERIFIER=on`、Tier B、WEB/Expr/Draw，并明确注入 InMemory SessionStore。生产持久会话使用
`--session sqlite --session-db /secure/path/sessions.sqlite`。`AGENT_FS_ROOT`、
`AGENT_TOOL_ALLOWLIST`、`AGENT_MCP_SKIP_SERVICES` 与可选 `AGENT_SERVER_AUTH_TOKEN` 都应在部署中显式设置。

- **`TaskControl`**：协作式 **`request_cancel` / `is_cancel_requested`**，以及 **`arm_working_deadline` / `check_deadline_now` / `is_deadline_exceeded`**（进入 `WORKING` 时由 Server 根据超时配置 `arm`）。
- **`AgentLoopNode::create`** 与 **`build_cli_agent_graph`** 可通过 **`CliAgentGraphOptions::task_control`** 将同一指针注入循环体，在迭代边界观察取消/超时（见 `src/node/agent_loop_node.cpp`）。

### 单任务超时（`metadata`）

- 若 **`SendMessage` / Legacy `metadata`** 中存在 **`timeout_sec`**（整数，秒），则优先于 **`AGENT_TASK_DEFAULT_TIMEOUT_SEC`**；再经 **`AGENT_TASK_MAX_TIMEOUT_SEC`** 钳制。
- 计时自 **`PENDING → WORKING`** 起；超时触发 **`WORKING → FAILED`**，并在 **`task.metadata["a2a_failure_reason"]`** 写入字面 **`timeout`**（随 `task_to_a2a_wire` 的 `metadata` 透出）。

### `CancelTask` 语义

- 未知任务 id：JSON-RPC **invalid params**（与 `GetTask` 一致）。
- 已为 **终态**（`COMPLETED` / `FAILED` / `CANCELLED`）：**幂等**返回当前任务 wire，**不**改状态，**不**额外 `push`。
- **`PENDING` / `INPUT_REQUIRED`**：立即 **`CANCELLED`**（若合法迁移），**`push_task_status_update`**，并从内部 `task_controls_` 摘除（队列内任务在 worker 入口见终态即不再执行图）。
- **`WORKING`**：仅置取消标志；终态与 SSE 在 worker 协作退出后落地。

### Termination precedence（worker 尾部）

在 GraphExecutor 的 future 完成后，Server 按序判定：**取消** 优先于 **超时**；二者优先于图返回的 **`COMPLETED`**。取消、超时、Verifier abort 或 revision conflict 均不提交 session 工作副本。

## JSON-RPC 方法支持矩阵（WP2.2 / WP2.3）

| method | 状态 |
|--------|------|
| `SendMessage` | 已实现；任务异步投递 |
| `GetTask` | 已实现 |
| `CancelTask` | WP2.3：协作式取消 + 幂等终态；`PENDING` 立即 `CANCELLED` + SSE |
| `ListTasks` | 空列表占位 |
| `SendStreamingMessage` | 已实现；创建任务并返回标准 SSE `StreamResponse` |
| `SubscribeToTask`（JSON-RPC 流） | 已实现；快照重放、增量事件、终态关闭 |

## 客户端与 SSE（cpp-httplib）

`AgentClient::send_streaming_task` 和 `subscribe_task_updates` 在非 legacy 模式使用 JSON-RPC POST SSE。
兼容 GET 端点仍保留一个迁移周期。参见 `test_phase2_a2a_execution` 与 `test_agent_server_wp22`。

每个订阅者使用独立有界队列。生产任务线程不会等待慢客户端；队列满时丢弃最旧帧，达到
`AGENT_SERVER_SSE_MAX_DROPPED` 后停止接收新帧，并在排空现有帧后关闭连接。该策略不会影响
同一任务的其他订阅者。

SSE 使用 A2A `StreamResponse` 根对象：answer/thinking 增量为 `message`（`metadata.streamChannel` 和
`metadata.append=true`）；工具、Verifier、Memory 等进度为 `statusUpdate.metadata`；工件为
`artifactUpdate`。`AgentClient` 将 `message` 增量呈现为携带单段 agent message 的 `WORKING` 更新。

## 相关代码

- [agent_server.hpp](../../include/agent/agent_server/agent_server.hpp)、[agent_server.cpp](../../src/agent_server/agent_server.cpp)
- [task_state_machine.hpp](../../include/agent/agent/task_state_machine.hpp)（`TaskControl` + `try_transition`）
- [task_dispatch_queue.hpp](../../include/agent/internal/task_dispatch_queue.hpp)
- [sse_server_channel.hpp](../../include/agent/internal/sse_server_channel.hpp)

## M2 前置

M2 happy path 另需客户端与契约快照等（WP2.4–2.6）；WP2.3 状态机/取消/超时已在本服务进程内落地（`test_task_state_machine`、`test_agent_server_wp23`）。
