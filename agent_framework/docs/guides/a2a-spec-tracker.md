# A2A 规范锚点（spec tracker）

**路径**：`agent_framework/docs/guides/a2a-spec-tracker.md`（固定，供 WP2.1 / WP2.2 / WP2.4 引用）  
**范围**：WP2.1a：**Agent Card（Well-Known）** 与 **JSON-RPC 2.0 通用层**；WP2.1：**方法表、Task/Message/Artifact ProtoJSON 映射、SSE 载荷、dispatch_table**。

**规范正文权威**：以 [A2A Specification v1.0.0](https://a2a-protocol.org/v1.0.0/specification) 及上游仓库 **`specification/a2a.proto`**（package `lf.a2a.v1`）的 **ProtoJSON（camelCase 字段名）** 为准。本文件为仓库实现对照表；若与网站示例冲突，以 **proto** 为准。

---

## 1. 对齐版本

| 项 | 值 |
|----|-----|
| 规范名称 | Agent2Agent (A2A) Protocol |
| 对齐版本 | **1.0**（Proto `protocol_version` 示例 `"1.0"`） |
| 对齐日期 | 2026-04-05 |
| 主文档 | [A2A Specification v1.0.0](https://a2a-protocol.org/v1.0.0/specification) |
| 规范源文件（字段名） | `specification/a2a.proto`（[A2A GitHub](https://github.com/a2aproject/A2A)） |
| Agent Discovery | [Agent Discovery](https://a2a-protocol.org/dev/topics/agent-discovery/) |
| v0.3 → v1 变更摘要 | [What's New in v1.0](https://a2a-protocol.org/latest/whats-new-v1/) |

---

## 2. 传输与 HTTP 绑定

| 项 | 本仓库锁定策略 |
|----|----------------|
| 字符编码 | **UTF-8**，**无 BOM** |
| Well-Known Agent Card | **HTTP GET**，路径 **`/.well-known/agent-card.json`**（RFC 8615） |
| JSON-RPC HTTP | **单一路径**：**POST** 至 Agent Card 声明的 **JSON-RPC 基 URL**（与 **Helloworld / 单接口** 示例一致：无 per-method path）。**`params` 必须为 object**（见 §8）。 |
| JSON-RPC 路径（本仓库实现） | 默认从 **`AgentCard::api_endpoint` 的 URL 路径** 推导；空路径则为 **`/`**。环境变量 **`AGENT_SERVER_JSON_RPC_PATH`** 非空时覆盖。 |
| JSON-RPC HTTP 状态码 | **一律 HTTP 200** 承载 JSON-RPC 信封（含 `error`）；与常见 JSON-RPC over HTTP 一致。 |
| SubscribeToTask（HTTP 侧，WP2.2） | 本仓库选用 **legacy 对照** 路径：**`GET /tasks/sendSubscribe?task_id=<id>`**（tracker §7）；规范 **`GET /tasks/{id}:subscribe`** 留待 WP2.4 等与客户端一并扩展。 |
| Content-Type | 请求/响应 JSON 正文由 WP2.2 设为 `application/json` |
| HTTP+JSON / gRPC | 规范另有 **`/message:send`、`GET /tasks/{id}`** 等绑定（见 `a2a.proto` `google.api.http`）。本仓库 **主目标**为 JSON-RPC 绑定；REST 路径为 **legacy**（见 §7）。 |

---

## 3. JSON-RPC 方法表

**method 字符串** 与 `A2AService` RPC 名一致（PascalCase），**params** 为对应 `*Request` 消息的 **ProtoJSON 对象**（camelCase 字段）。**result** 为对应响应消息的 ProtoJSON（或 `google.protobuf.Empty` 映射的空对象，以 SDK 为准）。

| method | params（必填摘要） | result（摘要） | 备注 |
|--------|-------------------|----------------|------|
| `SendMessage` | `message`（Message 对象）；可选 `tenant`、`configuration`、`metadata` | `SendMessageResponse`：`task` **或** `message` 二选一 | 规范 §3.1.1 |
| `SendStreamingMessage` | 同 `SendMessage` | **流**：多个 `StreamResponse`（见 §5）；首帧可为 `task` 或 `message` | 非单 JSON-RPC 响应体；由 WP2.2 与传输层处理 |
| `GetTask` | `id`（string）；可选 `tenant`、`historyLength` | `Task` | |
| `ListTasks` | 可选 `tenant`、`contextId`、`status`、`pageSize`、`pageToken`、`historyLength`、`statusTimestampAfter`、`includeArtifacts` | `ListTasksResponse`：`tasks`、`nextPageToken`、`pageSize`、`totalSize` | |
| `CancelTask` | `id`；可选 `tenant`、`metadata` | `Task` | |
| `SubscribeToTask` | `id`；可选 `tenant` | **流**：`StreamResponse`（见 §5） | HTTP+JSON 为 `GET .../tasks/{id}:subscribe`；JSON-RPC 流形态以规范 §9 为准 |
| `CreateTaskPushNotificationConfig` | `TaskPushNotificationConfig`（ProtoJSON，含 `taskId` 等） | `TaskPushNotificationConfig` | |
| `GetTaskPushNotificationConfig` | `taskId`、`id`；可选 `tenant` | `TaskPushNotificationConfig` | |
| `ListTaskPushNotificationConfigs` | `taskId`；可选 `tenant`、`pageSize`、`pageToken` | `ListTaskPushNotificationConfigsResponse` | |
| `DeleteTaskPushNotificationConfig` | `taskId`、`id`；可选 `tenant` | `Empty`（ProtoJSON 通常 `{}`） | |
| `GetExtendedAgentCard` | 可选 `tenant` | `AgentCard` | 需认证；见规范 §3.1.11 |

**A2A 业务错误**：v1.0 推荐在 JSON-RPC `error.data` 中携带 `google.rpc.ErrorInfo`（`reason` 为 UPPER_SNAKE_CASE，`domain`: `a2a-protocol.org`）。**WP2.1** 仅要求 **JSON-RPC 标准码**（§8）；业务 `reason` 由 WP2.2/WP2.3 填充并在后续迭代对齐规范。

---

## 4. Agent Card（Well-Known JSON）

### 4.1 与 v1.0 `AgentCard`（proto）的差异说明（重要）

**规范 v1.0**（`a2a.proto`）：Card **无**顶层 `url`；代之以 **`supportedInterfaces[]`**（`url`、`protocolBinding`、`protocolVersion`、`tenant`）。

**本仓库 WP2.1a 当前实现**（`wire_card` / `card_min.json`）：仍支持 **兼容形态**——顶层 **`url`** + `capabilities` 对象 + `defaultInputModes` 等（接近 v0.3 教程与部分 SDK 示例），便于与现有 `AgentCard::api_endpoint` 对齐。

| 轨迹 | 说明 |
|------|------|
| **WP2.1a（现状）** | `agent_card_from_a2a_wire` 以 **§4.2–4.4 表**为准（含顶层 `url`） |
| **WP2.x（后续）** | 增加 **`supportedInterfaces[0].url`** 解析，与 v1.0 完全对齐；tracker 届时更新必填键表 |

### 4.2 兼容形态：顶层键（WP2.1a `from_a2a_wire` 必填）

| 官方键 | 类型 | 说明 |
|--------|------|------|
| `name` | string | |
| `description` | string | |
| `url` | string | JSON-RPC 基 URL（兼容层） |
| `version` | string | Agent 实现版本 |
| `capabilities` | object | |
| `defaultInputModes` | string 数组 | |
| `defaultOutputModes` | string 数组 | |
| `skills` | array | 可为空 |

### 4.3 `agent_framework::AgentCard` ↔ 兼容层键映射

（同 WP2.1a：`api_endpoint` ↔ `url`，`capabilities` 布尔对象 ↔ `vector<string>`，等。）

### 4.4 `AgentSkill`、未知键、`defaultInputModes` / `defaultOutputModes`

（同 WP2.1a 初稿 §4.4–4.6。）

---

## 5. SSE / 流式载荷（`StreamResponse`）

### 5.1 语义

- **绑定**：`SendStreamingMessage`、`SubscribeToTask` 等的 **流** 中，每一则 **SSE `data:` 行**（或等价分帧）承载 **一个** `StreamResponse` 的 ProtoJSON 对象。
- **`StreamResponse`（oneof，四选一键）**（proto → JSON 字段名）：
  - `task` → **Task**
  - `message` → **Message**
  - `statusUpdate` → **TaskStatusUpdateEvent**（嵌套对象，内含 `taskId`、`contextId`、`status`）
  - `artifactUpdate` → **TaskArtifactUpdateEvent**（内含 `taskId`、`contextId`、`artifact`、`append`、`lastChunk` 等）

**规范锚点**：`a2a.proto` `message StreamResponse`。

### 5.2 W3C SSE 帧

- 由 `sse_framing.hpp` 实现：`event:`（可选）、`data:`（可多行拼接）、`id:`（可选）、空行结束。
- **Last-Event-ID**：客户端重连时携带 HTTP 头；**WP2.1** 仅解析 `id:` 字段填入 `SseEvent::id`（WP2.4 消费）。

### 5.3 与 legacy 事件名（对照）

| legacy（本仓库旧注释 / stub） | v1.0 `StreamResponse` 键 |
|------------------------------|---------------------------|
| `task_status_update` / 包裹 `task` | `statusUpdate`（或首帧直接 `task`） |
| `artifact_update` | `artifactUpdate` |

WP2.2 起 **禁止** 再向线路上发送无 `StreamResponse` 包裹的自定义 `kind` 字段（v0.3 风格）。

### 5.4 Identity 与审计（实现约定，交叉引用 v2）

[plan-detailed.v2.md](./plan-detailed.v2.md) §9 建议将 `tenant_id` / `agent_id` / `session_id` / `task_id` 写入日志与 SSE。**规范 ProtoJSON** 已含 `contextId`、`taskId` 等；**禁止**在标准字段上复用非规范语义。

| 需求 | 策略 |
|------|------|
| 官方已覆盖 | 使用 `Task.id`、`Task.contextId`、`Message.taskId`、`TaskStatusUpdateEvent.taskId` 等 |
| 额外审计字段 | 仅放在 **`metadata`（Struct）** 或 **`x-` 前缀** 自定义键（不进入规范必填路径）；与 WP2.7 `ExecutionContext` 衔接 |

---

## 6. Task / Message / Part / Artifact / TaskStatus（ProtoJSON ↔ `types.hpp`）

**Wire 名称** 均为 **camelCase**（ProtoJSON）。**实现函数**：`task_to_a2a_wire` / `task_from_a2a_wire` 等（`include/agent/a2a/wire_mapping.hpp`）。

### 6.1 `Task`

| ProtoJSON 键 | `AgentTask` 字段 | 说明 |
|--------------|------------------|------|
| `id` | `task_id` | 必填 |
| `contextId` | `session_id` | 可选 |
| `status` | `status` + `updated_at` | 见 §6.4；嵌套 `TaskStatus` |
| `history` | `messages` | `Message` 数组 |
| `artifacts` | `artifacts` | |
| `metadata` | `metadata` | JSON object |
| （无对应） | `created_at` | **to_wire** 不写；**from_wire** 无则默认 `epoch` |

### 6.2 `Message`

| ProtoJSON 键 | `AgentMessage` 字段 |
|--------------|---------------------|
| `messageId` | `message_id`（**from_wire** 必填；**to_wire** 若空则写 `"_"`） |
| `role` | `role`（`ROLE_USER` / `ROLE_AGENT`） |
| `parts` | `parts` |
| `contextId`、`taskId`、`metadata`、`extensions`、`referenceTaskIds` | **WP2.1** **from_wire** 忽略未知键以外仅解析 `metadata` 入扩展或忽略（当前忽略除 `messageId`/`role`/`parts` 外大部分） |

### 6.3 `Part`（oneof：成员存在性判别）

| Wire 判别 | `AgentPart::Type` | 映射 |
|-----------|-------------------|------|
| 存在 `text` | TEXT | `text`；`mediaType` 可选 |
| 存在 `url` | FILE | `AgentFileInfo.uri` = url；`mime_type` ← `mediaType` |
| 存在 `raw`（base64 字符串） | FILE | 解码失败则跳过该 part（**from_wire** 不抛） |
| 存在 `data` | DATA | 直接绑定 `json` |

**to_wire**：仅输出 **一个** content 键 + 可选 `mediaType`、`filename`、`metadata`。

### 6.4 `TaskStatus` 与 `AgentTaskStatus` 映射

**Wire `state`** 为枚举名字符串：`TASK_STATE_SUBMITTED`、`TASK_STATE_WORKING`、…（见 `TaskState` enum）。

| Wire `state` | `AgentTaskStatus` |
|--------------|-------------------|
| `TASK_STATE_SUBMITTED` | `PENDING` |
| `TASK_STATE_WORKING` | `WORKING` |
| `TASK_STATE_COMPLETED` | `COMPLETED` |
| `TASK_STATE_FAILED` | `FAILED` |
| `TASK_STATE_CANCELED` | `CANCELLED` |
| `TASK_STATE_INPUT_REQUIRED` | `INPUT_REQUIRED` |
| `TASK_STATE_REJECTED` | `FAILED`（**有损**；可在 `metadata` 记 `x-a2a-original-state`） |
| `TASK_STATE_AUTH_REQUIRED` | `INPUT_REQUIRED`（**近似**） |
| `TASK_STATE_UNSPECIFIED` 或未知 | `FAILED`（**from_wire**） |

**to_wire**：反向表；`PENDING` → `TASK_STATE_SUBMITTED`。

**时间戳**：`TaskStatus.timestamp` 使用 **ISO 8601 UTC**，毫秒 3 位 + `Z`（与规范示例一致）。C++ 使用 `std::chrono::system_clock` 转换。

### 6.5 `Artifact`

| ProtoJSON 键 | `AgentArtifact` |
|--------------|-----------------|
| `artifactId` | `artifact_id` |
| `parts` | `parts` |
| `metadata` | `metadata` |
| `name`、`description`、`extensions` | **from_wire** 可忽略或部分进入 `metadata`（WP2.1 忽略） |
| （无 `taskId` 于 Artifact 内） | `task_id` 仅 C++ 侧；**to_wire** 不写；**from_wire** 保留原 `task_id` 或空 |

### 6.6 LLM / Tool → `AgentPart`（阶段 1 衔接建议，非 WP2.1 强制代码）

| 来源 | 建议 Part |
|------|-----------|
| Assistant 纯文本 | `text` + `mediaType: text/plain` |
| `ToolCallRequest`（结构化） | `data` + `mediaType: application/json` |
| 工具返回文件 URI | `url` + `mediaType` |

详见 [phase-1-plan.md](./phase-1-plan.md) §6。

---

## 7. 与 legacy REST、`AgentClient` 的差异（WP2.4 迁移清单）

| legacy（`agent_server.cpp` / `overview.md`） | v1.0 目标（JSON-RPC / HTTP+JSON） |
|---------------------------------------------|-----------------------------------|
| `GET /.well-known/agent-card`（无 `.json`） | `GET /.well-known/agent-card.json` |
| `POST /tasks/send`，body `{ message, metadata, session_id }` | `SendMessage`：`params.message` 内带 `contextId`；无顶层 `session_id` |
| `GET /tasks/get?task_id=` | `GetTask`：`params.id` 或 HTTP `GET /tasks/{id}` |
| `POST /tasks/cancel` | `CancelTask`：`params.id` |
| `POST /tasks/update` | 规范以 **SendMessage** 向既有 task 发 Message 为主（见规范 **UnsupportedOperation** 与任务状态） |
| `GET /tasks/sendSubscribe?task_id=` | `SubscribeToTask` / SSE（§5） |
| `AgentTask::to_json`：`task_id`、`status: "working"`（小写） | ProtoJSON：`id`、`status.state: "TASK_STATE_WORKING"` |
| `AgentPart`：`type: text/file/data` | Part：**无 `type`**，以 `text`/`url`/`raw`/`data` 成员判别 |
| `discover_agent`：`AgentCard::from_json`（`api_endpoint`） | **WP2.4**：Well-Known 走 `agent_card_from_a2a_wire`；长期支持 `supportedInterfaces` |

---

## 8. JSON-RPC 2.0 通用策略（与 phase-2-wp1.md §5 一致）

### 8.1 请求解析

- 输入：`std::string_view` body UTF-8，`Content-Type` 由调用方保证 `application/json`（WP2.2）。
- 步骤：
  1. `json::parse(body)` 失败 → **逻辑层** `make_parse_error_response`（**`id` 为 null**）。
  2. 根非 object → `-32600`。
  3. 缺 `jsonrpc` 或非 `"2.0"` → `-32600`。
  4. `method` 非 string → `-32600`。
  5. `params` 若存在且既非 object 也非 array → `-32600`。**A2A 绑定：`params` 若存在则必须为 object，array → `-32600`**。
  6. `id` 缺失 → **不支持 notification** → **`-32600`**。
  7. 根为 array（batch）→ **不支持** → **`-32600`，`id: null`**。

### 8.2 响应序列化

- 成功：`{"jsonrpc":"2.0","id":<同请求>,"result":<json>}`。
- 失败：`{"jsonrpc":"2.0","id":<同请求或null>,"error":{"code":...,"message":...,"data":?}}`。
- **`id` 复制**：number/string 类型保持不变。

### 8.3 方法分发（`dispatch_table`）

- `method` 不在表 → `-32601`。
- `params` 结构校验失败 → `-32602`（`message` 含字段名）。
- handler 未捕获异常 → `-32603`；`data` 可选 `{"detail":"..."}`（**勿**含堆栈）。

### 8.4 标准错误码

| code | 名称 |
|------|------|
| -32700 | Parse error |
| -32600 | Invalid Request |
| -32601 | Method not found |
| -32602 | Invalid params |
| -32603 | Internal error |

**与 WP2.7**：输入策略失败时 `message` 前缀 `input_policy_violation` 且 `code=-32602`（见 [phase-2-wp7.md](./phase-2-wp7.md)）。

---

## 9. 修订记录

| 日期 | 版本 | 说明 |
|------|------|------|
| 2026-04-05 | 0.1 | WP2.1a：§1–2、§4 Card、§8 JSON-RPC、初版 §7 |
| 2026-04-05 | 0.2 | **WP2.1**：§3 方法表、§5 `StreamResponse` SSE、§5.1 Identity、§6 ProtoJSON 映射与状态表、§7 legacy 对照、§4.1 v1 Card 差异说明 |
| 2026-04-05 | 0.3 | **WP2.2**：§2 增补 JSON-RPC 路径解析、HTTP 200 错误信封、`sendSubscribe` HTTP 绑定字面量 |
