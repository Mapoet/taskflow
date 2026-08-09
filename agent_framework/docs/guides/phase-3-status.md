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

## 当前源码与测试证据

| 能力 | 主要源码 | CTest | 默认门闩 |
|---|---|---|---|
| VectorStore/RAG | `src/vectorstore/`、`src/node/knowledge_base_node.cpp`、`src/node/agent_loop_node.cpp` | `vectorstore_wp31`、`rag_e2e_wp31`、`vectorstore_faiss_contract` | `phase3-offline` / `phase3-faiss` |
| Memory Assembly | `src/memory/memory_assembly.cpp`、`src/node/agent_loop_node.cpp`、`src/graph_executor/graph_executor.cpp` | `memory_assembly_wp32`、`memory_assembly_events_wp32` | `phase3-offline` |
| Memory compaction | `src/agent/memory_compaction.cpp`、`src/llm_client/*_adapter.cpp` | `memory_compaction_wp33`、`memory_assembly_events_wp32` | `phase3-offline` |
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

## 2026-08-09 验证记录

- Debug 默认后端：`ctest --test-dir build -L phase3-offline --output-on-failure`，10/10 通过。
- Release Faiss：`cmake --build /tmp/taskflow-faiss-fix1 --target test_vectorstore_faiss_contract` 后执行 `ctest -L phase3-faiss`；本地验证通过，CI 对应 `.github/workflows/ubuntu.yml` 的 `phase3-faiss-release`。
- `phase3-ui`：6/6 通过（包含启用 FTXUI 后的 console view 契约）；本轮未修改 UI 视觉实现，因此不产生新的视觉基线截图。
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
