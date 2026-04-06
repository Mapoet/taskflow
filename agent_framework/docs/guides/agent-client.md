# AgentClient（WP2.4）

本文档描述 `AgentClient` 的 **A2A JSON-RPC（默认）** 与 **Legacy REST** 双模式、环境变量、与 [agent-server.md](./agent-server.md)、[a2a-spec-tracker.md](./a2a-spec-tracker.md) 的对齐关系。

## 模式与环境变量

| 变量 | 默认 | 说明 |
|------|------|------|
| `AGENT_CLIENT_USE_LEGACY_REST` | `0` | `1`：走历史 REST 路径（`/tasks/send` 等）；`0`：**JSON-RPC**（method 见 tracker §3） |
| `AGENT_CLIENT_JSON_RPC_PATH` | 空 | 非空时覆盖 JSON-RPC POST 的 **path**（须以 `/` 开头或由实现补全）；空则使用 **`/`**（与 Server 默认一致） |
| `AGENT_CLIENT_SSE_LEGACY_PAYLOAD` | `0` | `1`：SSE `data` 仍解析根级 `type: task_status_update` 等旧形态 |

## `AgentClientOptions` 与构造时固化

- **`AgentClient(server_url, options)`**：`options` 中 **未设置** 的字段在 **构造时** 用当前环境变量填充一次，并写入实例（`use_legacy_rest_`、`json_rpc_path_`）；之后 **改环境变量不会影响** 已有实例。
- **显式字段优先**：例如 `options.use_legacy_rest = true` 可让该实例走 Legacy，而无需设置 `AGENT_CLIENT_USE_LEGACY_REST`。同一进程内多个 `AgentClient` 可使用 **不同** `AgentClientOptions`，互不依赖全局 env。
- **线程与 env**：不要在任意线程仍可能执行 `AgentClient` 工作（含 `send_task` 等触发的 `std::async`）时，对 `AGENT_CLIENT_USE_LEGACY_REST` / `AGENT_CLIENT_JSON_RPC_PATH` 调用 `setenv`/`unsetenv`/`putenv`，除非全进程串行化这些调用；单测与多模式并存场景请优先使用 **`AgentClientOptions`**。

## A2A 模式（默认）

- **RPC URL**：`join_url(server_url, json_rpc_path)`；**不**再使用 `agent_endpoint` 拼接 JSON-RPC path。调用方可传 `agent_endpoint=""`。
- **SendMessage / GetTask / CancelTask**：`params` / `result` 为 **ProtoJSON**；任务反序列化使用 `task_from_a2a_wire`（见 [wire_mapping.hpp](../../include/agent/a2a/wire_mapping.hpp)）。
- **SendMessage** 的会话：若提供 `session_id`，客户端写入 **`metadata["contextId"]`**，与 `AgentServer::jsonrpc_send_message` 一致。
- **SSE**：`GET …/tasks/sendSubscribe?task_id=<id>`（常量见 [client_config.hpp](../../include/agent/a2a/client_config.hpp)）；**不**使用 `agent_endpoint` 前缀（与 RPC 一致）。
- **错误**：JSON-RPC `error` 抛出 **`agent_framework::a2a::A2aRpcException`**（含 `code()` / `data()`）。

## Legacy REST（过渡）

- 路径仍为 `agent_endpoint + "/tasks/send"` 等（与 WP2.2 前实现一致）。
- **移除计划**：默认关闭 Legacy 后 **保留不少于 2 个小版本** 的实现与 CTest，再删除代码路径（与 [phase-2-wp4.md](./phase-2-wp4.md) 一致）。

## 与 Server / 常量漂移

- JSON-RPC **method 字符串**与默认 path 的**字面源**：[client_config.hpp](../../include/agent/a2a/client_config.hpp)（须与 [a2a-spec-tracker.md](./a2a-spec-tracker.md) §3 一致）。
- **共享 POST 实现**：`HTTPAgentTransport` 与 `AgentClient` 均使用 **`a2a_jsonrpc_post`**（[jsonrpc_client.hpp](../../include/agent/a2a/jsonrpc_client.hpp)）。

## 尚未 JSON-RPC 化的 API

以下服务端当前仅为 **Legacy HTTP**，客户端在 **A2A 模式下仍使用相同 REST URL**（需 Server 启用 `AGENT_SERVER_LEGACY_REST` 等）：

- `update_task` → `POST …/tasks/update`
- `set_push_notification` / `get_push_notification_config` → `…/tasks/pushNotification/…`

## `std::future` 语义

- 返回 `std::future` 的方法在 **被调用的成员函数内部**（调用线程）同步完成 HTTP，再返回 **已就绪** 的 future；`get()` / `wait()` 立即拿到结果。这样避免 libstdc++ 将 `deferred` 任务派发到线程池与 httplib/OpenSSL 产生交互问题。若需要与别的工作并行，请在调用方自行 `std::async` 包装。

## 认证（WP2.5）

- **`set_authentication`**：`bearer`、`api_key`、`api_key_query`；非法 `type` 或缺字段抛 `std::invalid_argument`。
- **`api_key_query`**：仅在 **GET**（discover、Legacy `get_task`、`pushNotification/get`、SSE URL）上追加 query；**JSON-RPC POST 不** 附加 query。
- 完整表与服务端 env 见 **[a2a-authentication.md](./a2a-authentication.md)**。

## 相关测试

- `ctest -R agent_client_a2a`（`tests/test_agent_client_a2a.cpp`，场景 C-1–C-4）。
- `ctest -R agent_client_auth_query`（`api_key_query` 与 discover GET）。
