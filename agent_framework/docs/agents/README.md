# Agent 实现对比文档（索引）

本目录收录 **agent_framework**（本仓库）、**humanus-cpp** 与 **Claude Code 客户端源码树** 的对照分析，便于架构决策与补齐能力时查阅。

## 三库路径约定


| 代号     | 说明                                      | 典型相对路径（与 `taskflow` 仓库同级时）    |
| ------ | --------------------------------------- | ----------------------------- |
| **AF** | 本仓库 Agent Framework                     | `taskflow/agent_framework/`   |
| **HU** | humanus-cpp                             | `../humanus-cpp/`             |
| **CC** | Claude Code 源码树（产品级 TS，非 Anthropic 服务端） | `../claude-code-source-code/` |


CC 仅分析**本地可读的客户端**编排、权限、上下文与工具管线；不涉及闭源云端 API 实现或商业条款。

## Agent Framework 运行配置（MCP）

- **[guides/cursor_mcp_json.md](../guides/cursor_mcp_json.md)** — 在 `~/.cursor/mcp.json` 上追加 MCP、`mcpServers` 字段说明、工具名 `服务名__工具名`、与 `AGENT_TOOL_ALLOWLIST` 的关系。

## 阅读顺序

0. **阶段 2/3 深化（架构目录）**：[../architecture/plan-detailed.v2.md](../architecture/plan-detailed.v2.md) — Verifier、记忆落盘、Faiss、动态 MCP 等拍板与工作包；总纲仍见 [plan-detailed.md](../guides/plan-detailed.md)。
1. **[comparison_three_agents.md](./comparison_three_agents.md)** — 总览、对比矩阵、结论与对 AF 的启示。
2. 按需深入分册：
  - [deep_dive_execution.md](./deep_dive_execution.md) — 技术流与并行
  - [deep_dive_interfaces.md](./deep_dive_interfaces.md) — 接口与可扩展性
  - [deep_dive_security.md](./deep_dive_security.md) — 文件安全与命令权限
  - [deep_dive_memory_context.md](./deep_dive_memory_context.md) — 记忆、上下文层级与观测

## 与 `vs_humanus.md` 的关系

- **[vs_humanus.md](./vs_humanus.md)**：早期 **AF ↔ humanus-cpp** 双库速览与改进建议。
- **本系列**：在双库基础上加入 **Claude Code**，并拆成执行 / 接口 / 安全 / 记忆等可维护分册；总览以 `comparison_three_agents.md` 为准。

## 术语简表


| 术语                     | 含义                                        |
| ---------------------- | ----------------------------------------- |
| ReAct                  | 推理（LLM）与行动（工具）交替的代理循环                     |
| tool_use / tool_result | Anthropic Messages API 中的工具调用块与结果块（CC 原生） |
| Function calling       | OpenAI 风格函数/工具 JSON（AF / HU 常用）           |
| ToolBus                | AF 中统一注册与路由本地工具、MCP 代理的总线                 |
| PlanningFlow           | HU 中带 `PlanningTool` 的多步计划驱动外层循环          |


