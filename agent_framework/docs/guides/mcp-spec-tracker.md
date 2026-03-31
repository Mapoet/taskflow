# MCP 规范锁定（agent_framework 实现跟踪）

**锁定日期**：2026-03-31  
**参阅**：[Model Context Protocol](https://modelcontextprotocol.io/)（以官方当前文档为准；变更须先改本文再改代码常量）

## 协议版本

| 字段 | 值 |
| --- | --- |
| `protocolVersion`（initialize） | `2024-11-05` |

## JSON-RPC 2.0 方法名（须与 `mcp_protocol.hpp` 一字不差）

| 用途 | method 字符串 |
| --- | --- |
| 握手 | `initialize` |
| 握手后通知 | `notifications/initialized` |
| 列举工具 | `tools/list` |
| 调用工具 | `tools/call` |

## stdio 传输帧（本实现唯一支持格式）

与 MCP 规范 **Streamable HTTP / stdio** 常用写法一致：每条消息前为 **Content-Length** 头 + **`\r\n\r\n`** + **UTF-8 JSON body**。

```
Content-Length: <N>\r\n\r\n<body>
```

- `<N>` 为 **body 的字节长度**（非字符数）。
- 客户端与服务端均按此组帧读写。

**已知差异**：若对端使用 NDJSON 而非 Content-Length，本客户端**不支持**（须在 tracker「差异」登记后再实现）。

## HTTP 传输

- **请求**：`POST`，`Content-Type: application/json`，body 为**单条** JSON-RPC 对象。
- **URL**：由构造参数 `base_url` 给出**完整** POST 目标（例如 `http://127.0.0.1:8080/mcp`）；本实现**不**自动追加 path。
- **可选头**：`Authorization`、`Mcp-Session-Id` 等由 `HttpMCPTransport` 构造参数注入；若响应含 `Mcp-Session-Id`，保存并在后续请求带回。

## tools/list 与 tools/call 结果字段（映射）

- `tools/list` → `result.tools`：每项含 `name`、`description`（可选）、`inputSchema`（可选）→ 映射为 `ToolMeta::schema`（OpenAI parameters 形态由服务端给出，本客户端不改写）。
- `tools/call` → `result`：含 `content`、`isError` 等 → **整段 `result` 作为 JSON 返回**给上层（含错误语义时由 `isError` 表达）。

## 环境变量

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `AGENT_MCP_REQUEST_TIMEOUT_MS` | `60000` | 单次 stdio 读或 HTTP 读超时 |

## 修订记录

| 日期 | 说明 |
| --- | --- |
| 2026-03-31 | 初稿：Content-Length stdio、四方法名、HTTP POST 约定。 |
