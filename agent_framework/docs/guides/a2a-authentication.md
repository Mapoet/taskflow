# A2A 认证（WP2.5）

本指南描述 **`AgentServer` 内置 AuthGate** 与 **`AgentClient` 凭证注入**：Bearer、API Key（Header / Query）、`securitySchemes` 与 `match_card`；**401**、**`WWW-Authenticate`**；Well-Known 公开策略；与自定义 `set_authentication_validator` 的组合。

**OAuth 2.0 设备码（RFC 8628）**：当前 **未实现**；`AgentClient::refresh_authentication` 仍为占位。

---

## 1. 威胁模型与默认行为

| 场景 | 行为 |
|------|------|
| 未设置 `AGENT_SERVER_AUTH_MODE`（视为 `off`）且未配置内置强制 | 内置 **放行**；若仍设置 `set_authentication_validator`，仅执行自定义校验（AND）。 |
| `AGENT_SERVER_AUTH_MODE=bearer` 等 | 按模式强制校验；缺少有效凭证 → **401**；Bearer 场景下响应带 **`WWW-Authenticate: Bearer`**。 |
| `match_card` | 从 **`AgentCard::authentication_scheme`**（wire 的 `securitySchemes`）解析 **有效要求**；**允许的密钥/令牌** 仍只来自 **环境变量**（见 §3）。解析失败且 **`AGENT_SERVER_STRICT_CARD_AUTH=1`** → 服务端 **`start` 失败**（`setup_routes` 抛异常）。 |
| 日志 | 实现侧 **禁止** 记录完整 token；诊断仅为长度或掩码后若干字符。 |

---

## 2. 服务端环境变量

| 变量 | 取值 / 说明 |
|------|-------------|
| `AGENT_SERVER_AUTH_MODE` | `off` \| `bearer` \| `api_key_header` \| `api_key_query` \| `match_card` |
| `AGENT_SERVER_BEARER_TOKEN` | 单个 Bearer 密钥 |
| `AGENT_SERVER_BEARER_TOKENS` | 逗号分隔多个密钥（与上一行可同时存在，合并列表） |
| `AGENT_SERVER_API_KEYS` | 逗号分隔 API Key 值（header 或 query 模式共用） |
| `AGENT_SERVER_API_KEY_HEADER` | Header 名，默认 `X-API-Key`（`api_key_header` 模式；`match_card` 的 header 名以 **Card** 为准） |
| `AGENT_SERVER_API_KEY_QUERY` | Query 参数名，默认 `api_key` |
| `AGENT_SERVER_CARD_PUBLIC` | 默认 `1`：`/.well-known/agent-card.json` **不**走内置 Gate；`0` 时与健康检查 **不同**，发现端点 **需认证** |
| `AGENT_SERVER_STRICT_CARD_AUTH` | `1`：`authentication_scheme` 解析失败则 **拒绝启动** |

**`/health`**：始终 **匿名**可访问（不参与 AuthGate）。

---

## 3. `securitySchemes` 子集（Card）

解析支持 OpenAPI 风格的 **子集**：

- `{"type":"http","scheme":"bearer"}` → Bearer
- `{"type":"apiKey","in":"header"|"query","name":"..."}` → API Key

多种 **不兼容** 方案并存时：非严格模式下降级为 **无要求**（并警告）；严格模式 → **启动失败**。

---

## 4. 客户端 JSON（`set_authentication`）

| `type` | 必填 | 行为 |
|--------|------|------|
| `bearer` | `token` | `Authorization: Bearer …` |
| `api_key` | `key_value`；可选 `key_name`（默认 `X-API-Key`） | 自定义头 |
| `api_key_query` | `key_value`；可选 `param_name`（默认 `api_key`） | **仅 GET** URL 追加 query；**JSON-RPC POST 不** 附加 query（避免密钥进 body 与代理日志） |

未知 `type` → `std::invalid_argument`。

---

## 5. 自定义校验器

`AgentServer::set_authentication_validator(std::function<bool(const a2a::AuthContext&)>)`：

- 收到 **小写头表** 与 **小写 query 键** 的 `AuthContext`
- 与内置关系：**内置通过 AND 自定义通过**

---

## 6. 相关链接

- [phase-2-wp5.md](./phase-2-wp5.md)  
- [agent-client.md](./agent-client.md)  
- [envs_status.md](./envs_status.md)  
- RFC 6750（Bearer）  
- RFC 8628（设备码，可选未来工作）
