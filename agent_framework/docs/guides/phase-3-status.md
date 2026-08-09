# Phase 3 实施状态与证据矩阵

**最后核对日期**：2026-08-09  
**实施基线**：[phase-3-update-05.md](./phase-3-update-05.md)  
**状态语义**：`[ ]` 未开始；`[~]` 组件完成；`[x]` 集成完成；`[!]` 已知阻塞。

## 状态总览

| 工作包 | 状态 | 已验证能力 | 尚未关闭的 DoD |
|---|---:|---|---|
| WP3.0 | `[~]` | 事实矩阵、历史 AgentServer 标记、`phase3-offline` 聚合标签 | 全部 Phase 2 文档逐项校准、CI job 分层 |
| WP3.1 | `[~]` | 内存 VectorStore、持久化、KB retrieval/citation | Faiss Release 构建、generation sidecar、双后端契约、RAG E2E |
| WP3.2 | `[~]` | 确定性 Memory Assembly 和 AgentLoop 初步接入 | 六类 slot 唯一入口、assembled/evicted event、组合测试 |
| WP3.3 | `[~]` | truncate/extractive/structured 选择和失败回退 | 注册表、隔离子 LLM、schema/cancel/timeout 完整测试 |
| WP3.4 | `[~]` | File/SQLite event、message、summary 基础恢复 | tenant/session generation、fsync、digest、损坏恢复、权限、GC |
| WP3.5 | `[~]` | Skills 生命周期；MCP stage/activate/drain/remove、lease、基础快照 | canonical 签名、rehydrate、可靠 generation、lease 所有权、revision pinning |

## 当前源码与测试证据

| 能力 | 主要源码 | CTest | 默认门闩 |
|---|---|---|---|
| VectorStore/RAG | `src/vectorstore/`、`src/node/knowledge_base_node.cpp` | `vectorstore_wp31` | `phase3-offline` |
| Memory Assembly | `src/memory/memory_assembly.cpp`、`src/node/agent_loop_node.cpp` | `memory_assembly_wp32` | `phase3-offline` |
| Memory persistence | `src/memory/memory_store.cpp` | `memory_store_wp34` | `phase3-offline` |
| Dynamic MCP lifecycle | `src/mcp_client/mcp_lifecycle.cpp`、`src/toolbus/toolbus.cpp` | `mcp_lifecycle_wp35` | `phase3-offline` |
| OAuth token provider | `src/agent_client/token_provider.cpp` | `token_provider_wp36` | `phase3-offline` |
| Tool effect journal | `src/toolbus/tool_effect_journal.cpp` | `tool_effect_journal_wp37` | `phase3-offline` |
| Audit sink | `src/agent/audit.cpp` | `audit_wp38` | `phase3-offline` |

聚合验证命令：

```bash
ctest --test-dir build -L phase2 --output-on-failure
ctest --test-dir build -L phase3-offline --output-on-failure
ctest --test-dir build -L phase3-faiss --output-on-failure
ctest --test-dir build -L phase3-live --output-on-failure
ctest --test-dir build -L phase3-ui --output-on-failure
```

`phase3-faiss`、`phase3-live` 和 `phase3-ui` 是非默认门闩；只有对应目标和运行环境存在时才应声明通过。

## Demo 单一事实源

产品 Demo 目标以 `agent_framework/CMakeLists.txt` 为准。面向 Agent 交互的保留入口为：

- `cli_agent_demo`
- `tui_agent_demo`
- `web_ui_demo`
- `imgui_agent_demo`
- `agent_server_demo`（仅 Live）

上述入口统一使用 `examples/common/agent_example_bootstrap.hpp` 提供的 `LiveRuntime` bootstrap。历史文档中的 mock-only Server 和已删除 Demo 只作为设计记录，不是当前产品接口。

## 迁移与回滚约束

- WP3.1、WP3.4、WP3.5 的持久化 schema 在 generation/manifest 协议完成前不得标记 `[x]`。
- 新格式必须提供 schema version、旧格式只读迁移或明确拒绝，以及上一 committed generation 回退。
- 动态 MCP 重启后不得自动发布工具；必须重新验证并显式 activate。
- 可选 Faiss、Live 网络和 UI 截图测试不得进入默认无密钥离线门闩。
