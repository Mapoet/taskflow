# Cursor 格式 `mcp.json` 与 Agent Framework

Agent Framework 的 `ToolBus::register_mcp_from_cursor_config` 读取 **与 Cursor IDE 相同顶层结构** 的配置文件，便于在现有 `~/.cursor/mcp.json` 上**继续追加**条目，无需单独维护第二份配置（也可用 `AGENT_MCP_CONFIG_PATH` 或 `--cursor-mcp-json` 指向副本）。

## 1. 顶层结构

```json
{
  "mcpServers": {
    "服务别名": { ... },
    "另一个服务": { ... }
  }
}
```

- **`mcpServers`**（必填）：对象。每个 **key** 是「服务名」`service_name`（任意字符串，建议仅用字母、数字、`-`、`_`）。
- 每个 **value** 必须是 JSON 对象，且满足下面 **HTTP** 或 **stdio** 两种形式之一。

## 2. 两种服务端形态（与实现严格对应）

实现逻辑见 `agent_framework/src/toolbus/toolbus.cpp` 中 `register_mcp_from_cursor_config`：

| 形态 | 识别条件 | 支持的字段 | 说明 |
|------|----------|------------|------|
| **HTTP** | 存在非空字符串字段 **`url`** | `url`（必填）、`headers`（可选，字符串→字符串） | `type` 字段**不参与分支**；Cursor 里写的 `"type": "http"` 可保留，只要同时有 **`url`** 即可。 |
| **stdio** | **无** `url`，且存在 **`command`** | `command`（必填）、`args`（可选，字符串数组）、`env`（可选，字符串→字符串）、`framing`（可选） | 子进程环境 = **当前进程环境** 与 `env` 合并（同名键由 `env` **覆盖**）。默认 framing 为现代 MCP **JSON Lines**；旧服务可写 `"framing": "content-length"`。 |

任一服务端条目在连接或 MCP handshake 失败时，会记入导入结果的 **failures**，**不阻断**其他服务的注册（best-effort）。

## 3. 注册后工具在 LLM 眼里的名字

每个远端 MCP 工具会注册为：

```text
<service_name>__<远端工具名>
```

示例：服务名为 `filesystem`，远端工具名为 `read_file`，则全名为 **`filesystem__read_file`**。调用 `ToolBus::call_tool` 或写入 `AGENT_TOOL_ALLOWLIST` 时必须用**完整名**。

## 4. 在现有 `~/.cursor/mcp.json` 上追加配置

**做法**：保留原有 `mcpServers` 内所有键值，在同一对象内**新增**键即可（JSON 不允许尾逗号）。

下面示例在**虚构的**默认配置上增加 **官方 Filesystem** 与 **Fetch**（路径与 token 请替换为你本机值；勿把真实密钥提交到 Git）。

```json
{
  "mcpServers": {
    "sequence-thinking": {
      "url": "https://example.invalid/mcp",
      "headers": {
        "Accept": "application/json, text/event-stream"
      }
    },
    "context7": {
      "command": "npx",
      "args": ["-y", "@upstash/context7-mcp"],
      "env": {}
    },
    "filesystem": {
      "command": "npx",
      "args": [
        "-y",
        "@modelcontextprotocol/server-filesystem",
        "/path/to/your/project",
        "/tmp/readable-scratch"
      ]
    },
    "fetch": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-fetch"]
    }
  }
}
```

现代 `@modelcontextprotocol/*`、Context7 和 Playwright Node 服务通常无需填写 `framing`。只有明确仍输出 `Content-Length:` 头的旧服务才增加：

```json
"framing": "content-length"
```

说明：

- **`@modelcontextprotocol/server-filesystem`**：`args` 中 **紧跟包名之后** 的每一项都是该 ref server 允许的根目录（与 [MCP servers 文档](https://github.com/modelcontextprotocol/servers) 一致）。请只填需要的目录，勿写 `$HOME` 整棵树 unless 你有意为之。
- **`@modelcontextprotocol/server-fetch`**：按需添加；部分环境需可访问外网。

你当前的 Cursor 条目（如仅含 `url` 的远端、或 `command`+`args` 的 `npx`）可全部保留；AF 与 Cursor 共用同一文件时，**两边会各自启动/连接**各自需要的进程，注意重复 stdio 服务可能被占用——若冲突可为 AF 单独复制一份 `mcp.json` 并用 `--cursor-mcp-json` 指向该副本。

## 5. 程序如何选取配置文件路径

优先级（与 `cli_agent_demo` / 测试一致，详见 [getting_started.md](./getting_started.md)）：

1. 命令行：`--cursor-mcp-json /abs/path/mcp.json`
2. 环境变量：`AGENT_TEST_CURSOR_MCP_JSON`（测试/脚本常用）
3. `ToolBus` 内部：`AGENT_MCP_CONFIG_PATH` 非空则用其路径
4. 否则：`$HOME/.cursor/mcp.json`（`HOME` 未设置时实现里仍有兜底的默认路径，见 `default_cursor_mcp_path()`）

跳过加载 MCP：`--no-cursor-mcp` 或 `AGENT_TEST_SKIP_CURSOR_MCP` / `AGENT_CLI_SKIP_CURSOR_MCP=1`。

## 6. `AGENT_TOOL_ALLOWLIST` 与 MCP

若设置 **`AGENT_TOOL_ALLOWLIST`**（逗号分隔工具名），则：

- **本地工具**（如 `add`）名须在列表中；
- **每个** MCP 代理工具须以 **`service_name__tool_name`** 形式列入，否则 `register_mcp_service` 在注册阶段会 **抛错**（见 `agent_framework/src/toolbus/toolbus.cpp`）。

调试可先**不设置** allowlist，用 `-v` / `AGENT_TEST_AGENT_LOOP_DEBUG=1` 确认已注册名称后，再收紧白名单。

## 7. 与 Humanus 示例的对应关系

`humanus-cpp/config/example/config_mcp.toml` 中的 `stdio` / `sse` 与本文对应关系：

| Humanus (TOML) | Agent Framework `mcp.json` |
|----------------|----------------------------|
| `type = "stdio"`, `command`, `args` | `command` + `args` (+ `env`) |
| `type = "sse"`, `host`, `port` 等 | 不能直接等同于 HTTP POST；优先使用服务端提供的 Streamable HTTP **`/mcp`** URL。仅有旧版 `/sse` 时需要 legacy SSE 客户端。 |

将已有 Humanus 配置迁到 Cursor JSON 时，应先确认 endpoint 是可接收 JSON-RPC POST 的 Streamable HTTP，而不是只接受 GET 的旧版 SSE。出现 `/sse` 加网络错误时，同时检查服务进程是否启动、端口是否监听，以及服务是否另行提供 `/mcp`。

## 8. 校验清单

- [ ] 文件为合法 JSON（无注释、无尾逗号）。
- [ ] 每个服务要么有 **`url`**，要么有 **`command`**。
- [ ] stdio 的 `command` 在 `PATH` 中可找到（常用 `npx` 需已安装 Node）。
- [ ] 密钥优先放在受保护的环境变量或 `headers`，避免 URL 查询参数，并限制配置权限（如 `chmod 600 ~/.cursor/mcp.json`）。
- [ ] 使用 allowlist 时，预先用日志或 `export_as_llm_tools` 确认 **`服务名__工具名`** 拼写。
