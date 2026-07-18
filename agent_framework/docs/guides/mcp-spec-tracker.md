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
| 列举资源 | `resources/list` |
| 读取资源 | `resources/read` |

## stdio 传输帧

默认与现代 MCP SDK 一致，采用 **JSON Lines**：每条 UTF-8 JSON-RPC 消息必须在同一行，并以换行符结束。

```
<JSON-RPC object>\n
```

- stdout 只能承载协议消息；服务日志必须写 stderr。
- 单条消息上限为 16 MiB；分片 pipe 读取及一次读取中的多条消息均受支持。
- Cursor 风格 stdio 服务未配置 `framing` 时使用 JSON Lines。

历史服务可显式配置 `"framing": "content-length"`，使用旧格式：

```
Content-Length: <N>\r\n\r\n<body>
```

`<N>` 是 body 字节长度。不要依赖自动探测：客户端必须先选定请求的写入格式，对端才可能响应。

## HTTP 传输

- **请求**：`POST`，`Content-Type: application/json`，body 为**单条** JSON-RPC 对象。
- **URL**：由构造参数 `base_url` 给出**完整** POST 目标（例如 `http://127.0.0.1:8080/mcp`）；本实现**不**自动追加 path。
- **可选头**：`Authorization`、`Mcp-Session-Id` 等由 `HttpMCPTransport` 构造参数注入；若响应含 `Mcp-Session-Id`，保存并在后续请求带回。
- Streamable HTTP 的 POST 响应可以是 JSON 或 `text/event-stream`。旧版 MCP SSE transport 则是长期 `GET /sse`、服务端发布消息 POST endpoint 的双通道协议，不等同于本实现。
- 网络错误按连接、读、写、TLS 等类别报告；诊断不包含 URL 查询参数、header 或响应正文。

## tools/list 与 tools/call 结果字段（映射）

- `tools/list` → `result.tools`：每项含 `name`、`description`（可选）、`inputSchema`（可选）→ 映射为 `ToolMeta::schema`（OpenAI parameters 形态由服务端给出，本客户端不改写）。
- `tools/call` → `result`：含 `content`、`isError` 等 → **整段 `result` 作为 JSON 返回**给上层（含错误语义时由 `isError` 表达）。

## resources/list 与 resources/read 结果字段（映射）

- `resources/list` → `result.resources`：每项的 `uri`、`name` 必须是非空字符串；
  `description`、`mimeType`、非负 `size` 可选；`nextCursor` 原样交给调用方继续分页。
- `resources/read` → `result.contents`：每项必须含字符串 `uri`，并且恰好包含一个
  字符串 `text` 或 `blob`。客户端保留 `mimeType`，不会把 `blob` 隐式解码为文本。
- 只有服务在 `initialize.result.capabilities.resources` 中声明 Resources capability 后，
  客户端才允许调用上述方法。资源-only 服务不需要实现 `tools/list`。

## 环境变量

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `AGENT_MCP_REQUEST_TIMEOUT_MS` | `60000` | 单次 stdio 读或 HTTP 读超时 |
| `AGENT_MCP_RESOURCE_MAX_BYTES` | `262144` | 单次 `resources/read` 响应累计 text/blob 字节上限 |

## 修订记录

| 日期 | 说明 |
| --- | --- |
| 2026-03-31 | 初稿：Content-Length stdio、四方法名、HTTP POST 约定。 |
| 2026-07-17 | stdio 默认切换为规范 JSON Lines，Content-Length 改为显式 legacy 模式；补充 Streamable HTTP/SSE 边界。 |
| 2026-07-18 | 增加 Resources capability、`resources/list`、`resources/read`、资源-only 服务和响应大小边界。 |
