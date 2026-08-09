# Phase 3 实施状态与证据矩阵

**最后核对日期**：2026-08-09  
**实施基线**：[phase-3-update-05.md](./phase-3-update-05.md)  
**状态语义**：`[ ]` 未开始；`[~]` 组件完成；`[x]` 集成完成；`[!]` 已知阻塞。

## 状态总览

| 工作包 | 状态 | 已验证能力 | 尚未关闭的 DoD |
|---|---:|---|---|
| WP3.0 | `[x]` | 事实矩阵、历史计划标记、测试分层、Release Faiss CI、五个 Demo 单一事实源 | 无；Live/UI/Faiss 保持非默认门闩 |
| WP3.1 | `[x]` | Memory/Faiss generation 持久化与契约、GraphExecutor RAG E2E、metadata filter、稳定 citation | 无 |
| WP3.2 | `[x]` | 六类 slot 唯一 Assembly 边界、byte/token 双预算、确定性裁剪、精确 assembled/evicted event | 无 |
| WP3.3 | `[x]` | Compactor 注册表、隔离子 LLM、请求级 profile、schema/digest、timeout/cancel/fallback、精确事件 | 无 |
| WP3.4 | `[x]` | tenant/agent/session generation、fsync/digest、并发提交、损坏隔离与 CURRENT 修复、index 重建、GC、SQLite v3 迁移 | 无 |
| WP3.5 | `[x]` | canonical Ed25519、fail-closed、版本并存、tenant/session pin、rehydrate、trust revision、generation 冲突与回退、transport 校验 | 无 |
| WP3.6 | `[x]` | RFC 8628、加密 credential store、single-flight refresh、401 单次重试、SSE 换 token、route AuthGate | 无；真实 IdP 保持非默认 live 门闩 |
| WP3.7 | `[x]` | durable effect WAL、崩溃窗口恢复、严格迁移、Graph commit 边界、重复写阻断、人工复核 | 无；外部系统仍须实现 lookup/idempotency 契约 |
| WP3.8 | `[x]` | audit schema v2、redaction、JSONL/stderr/composite sinks、trace replay、renderer 慢告警 | 无；OTEL 仍是可选 adapter |
| WP3.9 | `[x]` | renderer registry、受控 worker、artifact store、恶意输入限制、plain fallback、真实 Web 截图 | 无；外部 raster worker 默认关闭 |

## 当前源码与测试证据

| 能力 | 主要源码 | CTest | 默认门闩 |
|---|---|---|---|
| VectorStore/RAG | `src/vectorstore/`、`src/node/knowledge_base_node.cpp`、`src/node/agent_loop_node.cpp` | `vectorstore_wp31`、`rag_e2e_wp31`、`vectorstore_faiss_contract` | `phase3-offline` / `phase3-faiss` |
| Memory Assembly | `src/memory/memory_assembly.cpp`、`src/node/agent_loop_node.cpp`、`src/graph_executor/graph_executor.cpp` | `memory_assembly_wp32`、`memory_assembly_events_wp32` | `phase3-offline` |
| Memory compaction | `src/agent/memory_compaction.cpp`、`src/llm_client/*_adapter.cpp` | `memory_compaction_wp33`、`memory_assembly_events_wp32` | `phase3-offline` |
| Memory persistence | `src/memory/memory_store.cpp` | `memory_store_wp34` | `phase3-offline` |
| Dynamic MCP lifecycle | `src/mcp_client/mcp_lifecycle.cpp`、`src/toolbus/toolbus.cpp` | `mcp_lifecycle_wp35` | `phase3-offline` |
| OAuth / AuthGate | `src/agent_client/token_provider.cpp`、`src/agent_client/agent_client.cpp`、`src/a2a/auth_gate.cpp` | `token_provider_wp36`、`oauth_retry_wp36`、`oauth_sse_wp36`、`auth_gate` | `phase3-oauth` / `phase3-offline` |
| Tool effect journal | `src/toolbus/tool_effect_journal.cpp`、`src/graph_executor/graph_executor.cpp` | `tool_effect_journal_wp37`、`tool_effect_graph_wp37` | `phase3-effects` / `phase3-offline` |
| Audit sink / trace | `src/agent/audit.cpp`、`src/graph_executor/graph_executor.cpp` | `audit_wp38`、`tool_effect_graph_wp37` | `phase3-observability` / `phase3-offline` |
| Rich renderer | `src/ui/rich_renderer.cpp`、`examples/web_ui_static/app.js` | `rich_renderer_wp39`、`web_ui_static_contract` | `phase3-renderer` / `phase3-ui` |

聚合验证命令：

```bash
ctest --test-dir build -L phase2 --output-on-failure
ctest --test-dir build -L phase3-offline --output-on-failure
ctest --test-dir build -L phase3-faiss --output-on-failure
ctest --test-dir build -L phase3-live --output-on-failure
ctest --test-dir build -L phase3-ui --output-on-failure
```

`phase3-faiss`、`phase3-live` 和 `phase3-ui` 是非默认门闩；只有对应目标和运行环境存在时才应声明通过。

## 2026-08-09 验证记录

- 完整默认离线门闩：`ctest --test-dir build -L phase3-offline --output-on-failure`，14/14 通过，覆盖 WP3.1–WP3.9。
- WP3.6–WP3.9 核心矩阵：OAuth/effects/observability/renderer 共 7/7 通过；`auth_gate` 1/1 通过。
- Release Faiss：`cmake --build /tmp/taskflow-faiss-fix1 --target test_vectorstore_faiss_contract` 后执行 `ctest -L phase3-faiss`；本地验证通过，CI 对应 `.github/workflows/ubuntu.yml` 的 `phase3-faiss-release`。
- `phase3-ui`：7/7 通过；真实 `web_ui_demo --demo-state` 在隔离端口完成 Markdown、KaTeX、Mermaid、工具状态和 artifact 加载验收。截图：[phase3-wp39-web-artifact-success.png](../assets/ui/phase3-wp39-web-artifact-success.png)。
- `phase2-*`、`phase3-offline`、`phase3-ui` 联合回归：25/25 通过。
- 五个 LiveRuntime 入口均在同一 Debug 配置中编译通过：`cli_agent_demo`、`agent_server_demo`、`tui_agent_demo`、`web_ui_demo`、`imgui_agent_demo`。
- `phase3-live` 需要外部端点/凭据，本轮不将无网络环境中的 transport 失败误报为功能回归。
- 历史 `phase-2-wp*.md` 已统一声明为历史计划，其中未勾选 checkbox 不再作为当前事实；本文件是完成状态的唯一事实源。

## Demo 单一事实源

产品 Demo 目标以 `agent_framework/CMakeLists.txt` 为准。面向 Agent 交互的保留入口为：

- `cli_agent_demo`
- `tui_agent_demo`
- `web_ui_demo`
- `imgui_agent_demo`
- `agent_server_demo`（仅 Live）

上述入口统一使用 `examples/common/agent_example_bootstrap.hpp` 提供的 `LiveRuntime` bootstrap。历史文档中的 mock-only Server 和已删除 Demo 只作为设计记录，不是当前产品接口。

## 迁移与回滚约束

- WP3.1、WP3.4、WP3.5 的持久化 schema 已使用 generation/manifest；升级时必须继续保留旧 schema 的显式迁移或拒绝策略。
- 新格式必须提供 schema version、旧格式只读迁移或明确拒绝，以及上一 committed generation 回退。
- 动态 MCP 重启后不得自动发布工具；必须重新验证并显式 activate。
- 可选 Faiss、Live 网络和 UI 截图测试不得进入默认无密钥离线门闩。
