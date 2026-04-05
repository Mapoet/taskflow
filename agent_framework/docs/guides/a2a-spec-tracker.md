# A2A 规范锚点（spec tracker）

**路径**：`agent_framework/docs/guides/a2a-spec-tracker.md`（固定，供 WP2.1 / WP2.2 / WP2.4 引用）  
**范围**：WP2.1a 落地 **Agent Card（Well-Known）** 与 **JSON-RPC 2.0 通用层**；任务 method、Task/Message wire、SSE 由 **WP2.1** 填充。

---

## 1. 对齐版本

| 项 | 值 |
|----|-----|
| 规范名称 | Agent2Agent (A2A) Protocol |
| 对齐版本 | **1.0.0**（与公开规范及 `a2a-sdk` 类型一致） |
| 对齐日期 | 2026-04-05 |
| 主文档 | [A2A Specification v1.0.0](https://a2a-protocol.org/v1.0.0/specification) |
| Agent Discovery（Well-Known） | [Agent Discovery](https://a2a-protocol.org/dev/topics/agent-discovery/) |
| 规范型定义参考 | [a2a.types.AgentCard](https://a2a-protocol.org/v1.0.0/sdk/python/api/a2a.types.html)（JSON 字段名以 **camelCase** 序列化为准） |

---

## 2. 传输与 HTTP 绑定

| 项 | 本仓库锁定策略 |
|----|----------------|
| 字符编码 | **UTF-8**，**无 BOM** |
| Well-Known Agent Card | **HTTP GET**，路径 **`/.well-known/agent-card.json`**（RFC 8615；与官方教程 `A2ACardResolver` 默认一致） |
| JSON-RPC HTTP | **单一路径**：**POST 至 Agent Card 的 `url` 字段所指向的 JSON-RPC 端点**（通常为服务基 URL，**无额外 path 后缀**；与官方 Helloworld 示例一致）。**不**采用「每方法不同 path」。 |
| Content-Type | 请求/响应 JSON 正文由 WP2.2 设为 `application/json` |

---

## 3. JSON-RPC 方法表

| method | params（摘要） | result（摘要） | 错误码 |
|--------|----------------|----------------|--------|
| （待 **WP2.1** 按规范逐行填入） | | | |

---

## 4. Agent Card（Well-Known JSON）

### 4.1 官方顶层键（与 a2a-sdk `AgentCard` 序列化一致，camelCase）

**必填**（本实现 `agent_card_from_a2a_wire` 若缺失则抛 `std::invalid_argument`）：

| 官方键 | 类型 | 说明 |
|--------|------|------|
| `name` | string | |
| `description` | string | |
| `url` | string | JSON-RPC 端点 URL |
| `version` | string | Agent 实现版本（非协议版本时仍为自由字符串） |
| `capabilities` | object | 见 4.3 |
| `defaultInputModes` | string 数组 | |
| `defaultOutputModes` | string 数组 | |
| `skills` | array | 可为空数组 |

**常用可选**：

| 官方键 | 类型 |
|--------|------|
| `provider` | object：`organization`（string）、`url`（string） |
| `protocolVersion` | string |
| `preferredTransport` | string（如 `JSONRPC`） |
| `securitySchemes` | object（OpenAPI 3.0 Security Scheme 风格 map） |
| `security` | array of object（OpenAPI Security Requirement） |
| `documentationUrl` | string |
| `iconUrl` | string |
| `supportsAuthenticatedExtendedCard` | boolean |
| `additionalInterfaces` | array |

### 4.2 `agent_framework::AgentCard` / `AgentSkill` ↔ 官方键映射

| C++ 字段 | 官方键 / 规则 |
|----------|----------------|
| `name` | `name` |
| `description` | `description` |
| `api_endpoint` | `url` |
| `provider` | `provider.organization`（若 wire 无 `provider`，则 C++ 置空字符串） |
| `capabilities`（`vector<string>`） | 由 `capabilities` **对象** 反推：`streaming`→`"streaming"`，`pushNotifications`→`"push-notifications"`，`stateTransitionHistory`→`"state-transition-history"`（为真则加入列表） |
| `authentication_scheme` | 整段 **`securitySchemes` 对象** 存入 `json`（无则 `null` / 空 object，见实现） |
| `skills[]` | `skills[]` 数组元素，见 4.4 |

### 4.3 `capabilities` 对象（官方）

| 官方键 | 类型 | 映射到 `vector<string>` |
|--------|------|---------------------------|
| `streaming` | boolean | 真 → 包含 `"streaming"` |
| `pushNotifications` | boolean | 真 → 包含 `"push-notifications"` |
| `stateTransitionHistory` | boolean | 真 → 包含 `"state-transition-history"` |
| `extensions` | array | **WP2.1a 不映射**到 C++ 字段（见未知键策略） |

### 4.4 `AgentSkill` ↔ 官方 skill 对象

| 官方键 | C++ `AgentSkill` 字段 |
|--------|------------------------|
| `id` | 不单独存储；**`to_wire`** 用 `name` 作 `id`（若需稳定 id 可后续扩展类型） |
| `name` | `name` |
| `description` | `description` |
| `tags` | `required_capabilities` |
| `inputModes` | 不恢复 schema；**`from_wire`** 时 `input_schema` 置空对象 `{}` |
| `outputModes` | 同上，`output_schema` 置 `{}` |
| `examples` | **WP2.1a 不映射**（忽略） |

**`to_wire`**：`inputModes` / `outputModes` 若 C++ 侧 `input_schema`/`output_schema` 非空对象则填 `["application/json"]`，否则 `["text/plain"]`。

### 4.5 未知键与扩展

- **策略（二选一已锁定）**：对 **未在 4.1–4.4 列出的顶层键**，`from_a2a_wire` **静默忽略**；`to_a2a_wire` **不输出**（不尝试保留未知数据）。  
- **扩展字段**：规范允许额外信息时，应使用 **明确命名空间前缀**（如 `x-` 前缀键）或独立注册扩展；**本实现 WP2.1a 不解析扩展语义**，仅因忽略策略而不报错。

### 4.6 `defaultInputModes` / `defaultOutputModes`（C++ 无对应字段）

- **`to_wire`**：始终输出默认 `["text/plain"]`（除非后续 WP 为 `AgentCard` 增加字段）。  
- **`from_wire`**：键**必填**；值不参与 C++ 模型，仅校验存在且为字符串数组。

---

## 5. SSE / 任务事件

**WP2.1** 填充：事件名、`data` JSON 形状、与 `sse_framing` 对齐。

---

## 6. 与当前 `AgentClient::discover_agent` 的差异

| | 当前实现 | 官方 Well-Known Card（本 tracker） |
|--|----------|-------------------------------------|
| Body 形状 | `AgentCard::from_json` 期望键如 `api_endpoint`、`authentication_scheme`、`skills[].input_schema` | camelCase：`url`、`securitySchemes`、`capabilities` 对象、`defaultInputModes` 等 |
| 迁移 | **WP2.4**：发现结果解析改为 `agent_card_from_a2a_wire` | |

---

## 7. JSON-RPC 2.0 通用策略（与 phase-2-wp1.md §5 一致）

以下内容与 [phase-2-wp1.md](./phase-2-wp1.md) **§5** 一致，作为本 tracker 权威条文：

### 5.1 请求解析

- 输入：`std::string_view` body UTF-8，`Content-Type` 由调用方保证 `application/json`（WP2.2）。
- 步骤：
  1. `json::parse(body)` 失败 → **逻辑层** `make_parse_error_response`（**`id` 为 null**）。
  2. 根非 object → `-32600`。
  3. 缺 `jsonrpc` 或非 `"2.0"` → `-32600`。
  4. `method` 非 string → `-32600`。
  5. `params` 若存在且既非 object 也非 array → `-32600`。**本 tracker 锁定 A2A 绑定：`params` 若存在则必须为 **object**，**array 视为 `-32600`**。
  6. `id`：缺失 → **notification**；**不支持 notification** → **缺 `id` 一律 `-32600` Invalid Request**。
  7. 根为 array → **batch 不支持** → **单一错误响应 `-32600`，`id: null`**。

### 5.2 响应序列化

- 成功：`{"jsonrpc":"2.0","id":<同请求>,"result":<json>}`。
- 失败：`{"jsonrpc":"2.0","id":<同请求或null>,"error":{"code":...,"message":...,"data":?}}`。
- **`id` 复制**：number/string 类型保持不变。

### 5.3 方法分发（WP2.1 / dispatch_table）

- `method` 不在表 → `-32601`。
- `params` 结构校验失败 → `-32602`。
- handler 逻辑异常 → `-32603`。

### 5.4 标准错误码（JSON-RPC）

| code | 名称 |
|------|------|
| -32700 | Parse error |
| -32600 | Invalid Request |
| -32601 | Method not found |
| -32602 | Invalid params |
| -32603 | Internal error |

---

## 8. 修订记录

| 日期 | 版本 | 说明 |
|------|------|------|
| 2026-04-05 | 0.1 | WP2.1a：§1–2、§4 Card 映射、§5 JSON-RPC、§6 与 Client 差异 |
