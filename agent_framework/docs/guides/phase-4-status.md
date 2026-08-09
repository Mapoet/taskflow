# Phase 4 实施状态与证据矩阵

**状态**：实施前基线  
**最后核对日期**：2026-08-09  
**上游章程**：[phase-4.md](./phase-4.md)  
**总体计划**：[phase-4-plan.md](./phase-4-plan.md)

## 1. 状态语义

| 标记 | 含义 | 允许的证据 |
|---|---|---|
| `[ ]` | 尚无可复用实现 | 只有需求或设计文档 |
| `[~]` | 已有基础或组件，但未形成 Phase 4 集成闭环 | 源码、组件测试或历史功能 |
| `[x]` | 当前工作包 DoD 全部关闭 | 代码、五层测试、运行证据、文档和门禁 |
| `[!]` | 存在已确认阻塞 | 阻塞事实、影响、替代方案和解除条件 |

“满足度”是按接口、组件、集成、真实运行证据、生产门禁五级评估的工程成熟度，不是代码覆盖率。任何 `[x]` 必须同时在[追溯矩阵](./phase-4/traceability-matrix.md)和[执行台账](./phase-4/execution-ledger.md)中有证据。

## 2. 当前总体判断

Phase 4 当前总体满足度约为 **25–30%**。Phase 3 已提供可靠的单机 Agent Runtime 基座，但 Phase 4 的认知规划、多层记忆与动态 View、全运行持久化、durable approval、五层验收、标准遥测、评测集和分布式控制面尚未形成。

| 工作包 | 状态 | 满足度 | 已有基础 | 未关闭的核心 DoD |
|---|---:|---:|---|---|
| WP4.0 Task Cognition | `[~]` | 25% | 输入预处理、RAG、ToolBus、ChildTask、Subflow、A2A | 无 TaskIntake/Evidence/Understanding/Plan DAG/critic/revision |
| WP4.1 Assurance Harness | `[~]` | 20% | Verifier、CTest、Audit、artifact、UI 验证惯例 | 无 AcceptanceContract、五层 verifier、独立 arbiter、证据充分性 |
| WP4.2 Durable Run | `[~]` | 35% | SessionStore、checkpoint、Effect WAL、memory generation、child resume | 无统一 RunStore、graph cursor、timer/retry/interrupt、replay/time-travel |
| WP4.3 HITL/Policy | `[~]` | 20% | cancel/deadline、Skill Policy、Tool hook、ManualReview | 无 durable approval、reviewer/decision、参数编辑、恢复和 UI queue |
| WP4.4 Sandbox Runtime | `[~]` | 45% | FS jail、bwrap/unshare、networkless Skill、RLIMIT、renderer worker | 无统一 provider、session workspace、container/remote、credential broker |
| WP4.5 OTel/SLO | `[~]` | 35% | AuditEvent、trace ID、JSONL/stderr、latency、ExecutionEvent | 无标准 span/OTLP/metrics/cost/critical path/SLO |
| WP4.6 Evaluation | `[~]` | 15% | CTest、fixture、Phase labels、Verifier tests | 无任务集、trajectory、质量指标、模型矩阵、judge calibration |
| WP4.7 Live Certification | `[~]` | 15% | 两个 opt-in A2A live test | 可 skip-as-pass；无 scheduled matrix 和环境 manifest |
| WP4.8 Distributed Control | `[~]` | 10% | 进程内 FIFO/worker、SQLite、A2A ChildTask | 无 durable queue、ownership lease、heartbeat、HA、远程存储 |
| WP4.9 Multi-layer Memory | `[~]` | 25% | MemoryStore/Assembly/Compaction、RAG、Skill、Session checkpoint | 无多层 namespace、authority/lifecycle、动态 View、scope-first retrieval、晋升/遗忘治理 |

WP4.9 使用尾部编号只是为了不重排已经冻结的 116 个任务 ID；它是 P0，并应在认知规划和五层验收垂直闭环之前达到可用状态。

## 3. WP4.0 当前证据

**可复用**：

- `include/agent/agent/user_input_types.hpp`：正文、注入块、控制动作和违规；
- `src/agent/user_input_preprocessor.cpp`：Tier A/B 输入处理；
- `src/node/agent_loop_node.cpp`：RAG、ToolBus、多轮 ReAct；
- `include/agent/agent/child_task.hpp`、`include/node/subflow_node.hpp`：有界子任务与子图；
- `src/a2a/orchestration.cpp`：远程 Agent 调度入口。

**判定**：这些能力支持调查和执行，但没有任务理解及计划领域模型；ReAct 中临时选择工具不能替代版本化专业计划。

## 4. WP4.1 当前证据

**可复用**：

- `include/agent/agent/verifier_types.hpp`：`ok/issues/suggested_action`；
- `src/agent/verifier_runner.cpp`：隔离的第二 LLM profile、timeout 和结构化输出；
- `include/agent/observability/audit.hpp`：trace、digest、redaction；
- Phase 2/3 CTest、A2A fixture、Faiss contract 和真实 Web 截图。

**关键限制**：现有 Verifier prompt 明确禁止调用工具或外部 API，只能检查用户任务与 draft answer；它只能成为 Assurance Harness 的一个语义 verifier，不能作为最终验收器。

## 5. WP4.2 当前证据

**可复用**：

- `SessionStore`：CAS revision、checkpoint ID、tool/child snapshots；
- `ToolEffectJournal`：Started/Completed/Committed 与 reconciliation；
- `MemoryStore`、Faiss、MCP registry：generation/manifest 恢复；
- Skill Workflow 和 ChildTask：checkpoint、retry/restart/resume。

**关键限制**：没有统一 RunState/RunStore；GraphExecutor 未持久化 node cursor、pending work、timer、interrupt、plan/acceptance revision 和 deterministic replay 元数据。

## 6. WP4.3 当前证据

**可复用**：

- `TaskControl`：cooperative cancel 与 deadline；
- Skill grant/policy、Tool hook 和 MCP fail-closed；
- Effect Journal 的 ManualReview 状态；
- AgentServer cancel API 与 SSE 状态更新。

**关键限制**：没有 ApprovalRequest/Decision schema、durable interruption、reviewer identity、expiry、delegation、参数编辑、双人复核和三端 UI pending queue。

## 7. WP4.4 当前证据

**可复用**：

- `FsSandboxConfig` 和 canonical path jail；
- Skill process 使用 `unshare + bwrap`、网络隔离、环境 allowlist、secret file、CPU/内存限制；
- Rich renderer 使用无 shell 子进程及 CPU/内存/文件/输出/超时限制。

**关键限制**：隔离逻辑分散在 Skill、FS 和 renderer；没有统一 SandboxProvider、workspace snapshot/diff、生命周期、container/remote provider、credential broker 和审计 manifest。

## 8. WP4.5 当前证据

**可复用**：

- Audit schema v2、JSONL/stderr/composite sinks；
- ExecutionEvent sequence 和 trace/task/session/run 关联；
- latency threshold、memory metrics、ChildTask token usage。

**关键限制**：没有 span parentage、OTLP exporter、标准 GenAI semantic convention、histogram、token/cost/cache/queue 指标、sampling、critical path、dashboard 和 SLO。

## 9. WP4.6 当前证据

**可复用**：默认构建中存在 3000 余个 CTest，其中 Agent Framework 已有 unit、contract、loopback、fault/recovery、UI 和 Phase 标签。

**关键限制**：没有独立 eval runner、dataset schema、trajectory recorder、模型/Prompt 版本矩阵、RAG/规划/验收质量指标、统计比较、flaky detection 或 judge calibration。

## 10. WP4.7 当前证据

`a2a_live_smoke` 和 `a2a_live_multi_agent` 提供 live 入口，但环境变量未设置时返回 0；普通绿色 CTest 不能证明实际执行。当前无强制 `executed=true` 证据、scheduled workflow、真实 IdP/MCP/LLM/renderer/sandbox 组合矩阵和环境 manifest。

## 11. WP4.8 当前证据

AgentServer 使用有界进程内 FIFO、worker threads、内存 active task map 和可选 SQLite SessionStore。A2A/ChildTask 支持远程执行，但没有 durable dispatch、任务所有权 lease、worker heartbeat、leader election、远程 artifact/evidence store、HA 和 rolling migration。

## 12. WP4.9 当前证据

**可复用**：

- `include/agent/memory/memory.hpp`、`src/memory/memory_store.cpp`：File/SQLite/InMemory、tenant/agent/session、generation、digest、恢复、redaction 和 GC；
- `include/agent/memory/memory_assembly.hpp`、`src/memory/memory_assembly.cpp`：System/Task/Working/Retrieval/Tool/Skill 六类统一装配、双预算、确定性淘汰和 citation 保留；
- `src/agent/memory_compaction.cpp`：结构化摘要、truncate/fallback、取消和报告；
- VectorStore/Faiss 与 KnowledgeBase：metadata filter、持久索引、RAG citation；
- SessionStore/ExecutionContext/AgentThreadState：session/task/tenant 和 checkpoint 基础；
- Skill Registry/Loader/Runtime：L1/L2 渐进披露、权限、来源、签名和 generation pin。

**关键限制**：当前六类 slot 是 Prompt 内容类别，不是系统/组织/个人/项目/任务/轮次作用域；`MemoryScope` 仅有 tenant/agent/session；没有 authority、trust、freshness、sensitivity、conflict、supersede/forget，RAG 未强制 scope-first，AgentLoop 只有单一静态装配路径，Run checkpoint 未 pin MemorySnapshot/View digest，工作记忆也没有受治理的跨层晋升机制。

## 13. 当前可复用测试入口

| 能力 | 代表测试 |
|---|---|
| 输入/认知基础 | `user_input_preprocessor_wp27`、`user_input_wp27_integration_i1` |
| 子任务/子图 | `phase2_child_task`、`agent_subflow`、`a2a_orchestrator_tools` |
| Verifier | `verifier_types`、`verifier_runner`、`verifier_graph_hooks_i1` |
| 持久化 | `phase2_session_store`、`tool_effect_journal_wp37`、`memory_store_wp34` |
| 沙箱/策略 | `skill_process_sandbox_contract`、`skill_policy_runtime_contract` |
| 审计 | `audit_wp38`、`tool_effect_graph_wp37` |
| Live | `a2a_live_smoke`、`a2a_live_multi_agent`（必须防止 skip-as-pass） |
| 多层记忆基础 | `memory_assembly_wp32`、`memory_store_wp34`、`memory_compaction_wp33`、`rag_e2e_wp31`、`prompt_renderer_skill_block` |

## 14. 状态维护规则

1. 状态只在本文件更新，工作包详案不重复声明“当前完成度”。
2. `[~] → [x]` 必须关闭该 WP 的所有 mandatory acceptance criteria。
3. 新测试只有在 CI 门禁实际执行且不可静默 skip 时才算生产证据。
4. 所有 schema 变化必须记录 decision、migration、rollback 和 fixture revision。
5. 每次实施后更新 source/test/evidence、最后验证日期和 plan revision。
6. 外部依赖不可用不等于通过；应记录 `[!]` 或 `inconclusive`。
