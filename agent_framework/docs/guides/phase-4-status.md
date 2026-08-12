# Phase 4 实施状态与证据矩阵

**状态**：Phase 4 v2 技术状态重估基线（未完成项保持开放）  
**Residual Closure**：[`phase4-v2-residual-r1`](./phase-4-residual-closure-plan.md) 已于 2026-08-11 批准执行；R1I–R7D 按直接集成/生产证据关闭，不以既有模块测试替代
**最后核对日期**：2026-08-12
**上游章程**：[phase-4.md](./phase-4.md)  
**当前总体计划**：[phase-4-plan-v2.md](./phase-4-plan-v2.md)  
**v1 历史计划**：[phase-4-plan.md](./phase-4-plan.md)

## 1. 状态语义

| 标记 | 含义 | 允许的证据 |
|---|---|---|
| `[ ]` | 尚无可复用实现 | 只有需求或设计文档 |
| `[~]` | 已有基础或组件，但未形成 Phase 4 集成闭环 | 源码、组件测试或历史功能 |
| `[x]` | 当前工作包 DoD 全部关闭 | 代码、五层测试、运行证据、文档和门禁 |
| `[!]` | 存在已确认阻塞 | 阻塞事实、影响、替代方案和解除条件 |

“满足度”是按接口、组件、集成、真实运行证据、生产门禁五级评估的工程成熟度，不是代码覆盖率。任何 `[x]` 必须同时在[追溯矩阵](./phase-4/traceability-matrix.md)和[执行台账](./phase-4/execution-ledger.md)中有证据。

## 2. 当前总体判断与双基线

本节采用两条需求基线、两条证据轴，避免再用一个百分比同时表达“代码已经存在”和“生产环境已经认证”：

- **v1 基线**：`phase-4.md` 的生产 Harness、durable run、HITL、sandbox、观测、评测、Live 与分布式控制目标；
- **v2 基线**：在 v1 上增加 LLM 驱动的任务认知/规划、多层记忆、专业验收、修复重规划、Judge 和解释性运维；
- **工程实现成熟度**：契约、实现、持久化、集成测试和本地故障恢复已经具备的比例；
- **生产认证成熟度**：真实身份、真实 provider/数据、不可绕过组合、外部调度、签名报告、跨进程/跨主机故障证据已经具备的比例。

截至 2026-08-12，v1 工程实现成熟度为 **77–81%**，v2 工程实现成熟度为 **84–87%**；Phase 4 整体生产认证成熟度为 **58–64%**。这些区间是按工作包 DoD 加权后的审计判断，不是代码覆盖率。前两项反映大量控制面和本地 durable 实现已经落地，后一项刻意扣除了 scripted RoleRuntime、deterministic UI snapshot、单机 SQLite、多连接模拟及 `executed=false` Live 报告。

当前已证明的最高层次是：共享且版本化的契约；LLM Role Runtime 及规划、记忆、验收、修复、Judge 工作流；durable Harness；RunStore 原子 checkpoint/event/effect/memory/interruption；审批治理；Bubblewrap deny-all sandbox；credential broker；W3C trace、OTLP spool 与 SLO；Eval campaign；SQLite/PostgreSQL distributed control；四端同源 operations projection。P4-PC0 又增加了 11 阶段 fail-closed production composition 和 adapter capability manifest；P4-PC2 增加 Run/Harness 跨库 saga journal、双侧 revision/digest pin、重启恢复及漂移人工复核；P4-PC3 增加六类 concrete workflow adapter、强类型输入 assembler、Harness pin 校验和 durable invocation manifest 反查；P4-PC4 增加统一、摘要寻址、revision-aware 的 durable ProductionWorkflowInputRepository；P4-PC6A/PC6I 增加完整 production dependency graph、Sandbox/LLM-store identity 启动校验、唯一默认 composition builder 以及 dependency/composition/deployment 三层 manifest pin。P4-COBS 又补齐生产只读 ToolBus Investigator、持久化 adaptive stop、确定性采样、correlated logs、OTLP 三信号、HTTPS CA/mTLS 客户端身份配置、三信号 durable spool，以及窗口化 error-budget/burn-rate 门禁。P4-SIC1–SIC8 继续补齐 checkpoint→Run/Harness saga 自动通知、持久化 plan approval resolver、四类标准 production boundary adapter、修复方案二次审批、旧证据失效、artifact lineage/复验，以及 spool backoff/dead-letter/retention 和 quantile/multi-window SLO。SIC9 于 2026-08-12 完成 `phase4-offline` **70/70 PASS**；这仍是本地控制面证据，不是 production Live 认证。

P4-PC3 起 production composition 只接受 `WorkflowHarnessStagePort`：adapter 必须给出强类型 kind/stage、实现 revision、配置 digest，并绑定强制 LLM invocation observer；普通 callback 和历史 `ManifestHarnessStagePort` 即使自报 manifest 也会被拒绝。Cognition、Memory、Professional Assurance、Remediation、Reverification、Judge 已有直接调用真实 workflow class 的 concrete adapter、强类型输入 assembler、Store-backed 组合器、Harness digest pin 校验及 durable LLM manifest 反查。P4-PC4 又补齐统一、摘要寻址、revision-aware 的 SQLite ProductionWorkflowInputRepository，覆盖 Intake/Memory/Acceptance/Artifact/Assurance/Impact/Eval 输入及 Eval registry 重建。但这些能力仍**不等于默认部署 wiring 或真实 provider 已认证**。修复后新产物回填、Approval/Run 跨 Store 协调、真实 IdP/KMS/provider 校准、外部 Live matrix，以及已暂缓的多节点 PostgreSQL/ObjectStore/Agent chaos 均保持开放。

| 基线 / 证据轴 | 当前值 | 已证明边界 | 仍未证明边界 |
|---|---:|---|---|
| v1 工程实现成熟度 | 76–80% | durable Harness/Run、HITL 控制面、sandbox、OTel/SLO、Eval、Live certification 控制面、distributed control | 全阶段生产 adapter、跨 Store saga、真实身份/签名/数据、跨主机 HA |
| v2 工程实现成熟度 | 84–87% | v1 能力，加 LLM cognition/planning、memory、professional assurance、remediation、multi-Judge、explainable operations，以及 dependency-validated 默认 production composition | 生产 calibration、真实边界 adapter 与闭环质量指标 |
| Phase 4 生产认证成熟度 | 58–64% | 本地真实进程/SQLite/PostgreSQL/TLS/截图证据，以及 fail-closed/no-skip 门禁 | 获批且未过期的 `executed=true` Live bundle、真实多 provider/IdP/KMS、跨主机 chaos |

因此，Phase 4 当前仍统一标记为 **`[~]`**。`[x]` 只在单个工作包的 mandatory DoD、追溯矩阵、执行台账和生产证据同时闭合时使用；不得用较高的 v2 工程实现百分比替代生产完成声明。

### 2.1 v2 横向能力状态

| 能力域 | 状态 | v2 满足度 | 当前事实 | 未关闭的 mandatory DoD |
|---|---:|---:|---|---|
| Role-based LLM Runtime | `[~]` | 70–75% | 已有版本化 Profile/Prompt/Route/Manifest/Reasoning/Calibration 契约、确定性 Router、schema gate、SQLite CAS/recovery、usage/latency、Telemetry/Audit bridge、fake fallback/repair 和可选 AgentLoop adapter | 独立 `StructuredOutputPolicy` 契约、project/org overlay/default revision 管理、GraphExecutor 全路径强制接入、timeout/live provider/calibration matrix |
| LLM Cognition/Planning | `[~]` | 78–84% | RoleRuntime stages、strict checkpoint、生产只读 ToolBus investigator、adaptive stop、synthesis/boundary/planner/独立 critic/revision/HITL、SQLite recovery、GraphExecutor 与默认 production adapter | 真实外部 investigator/provider calibration、跨 Store 原子提交、ApprovalStore 直连和 live/SLO |
| LLM Professional Assurance | `[~]` | 75–80% | Verification Planner；Manifest deterministic oracle；Code/Architecture/Domain/Security/Completeness 与 Resolver 七个 RoleRuntime 路由；只读 capability intersection；执行者/规划者隔离；evidence closure；强 oracle precedence；cross-verifier conflict；SQLite checkpoint/report 原子提交、restart 和 GraphExecutor | 真实 repo/build/runtime/domain/security/Sandbox adapters、双 verifier 策略配置、生产 calibration/quality benchmark、跨 Store 原子绑定、修复后自动复验、默认强制接入与 Live/UI |
| LLM Multi-layer Memory | `[~]` | 75–80% | 九阶段 RoleRuntime memory workflow、strict/SQLite checkpoint、scope-first/provider 二次隔离、candidate-only write、entity/claim linking、consolidation/conflict、task state、query/rerank、dynamic View、HITL promotion/forget、v1 dual-read 和 GraphExecutor 离线闭环 | 真实 profile/provider calibration、可执行 hybrid index、跨 Store 原子提交、ApprovalStore 直连、cross-scope migration、默认强制接入、Live/HA/Inspector |
| LLM Remediation/Replan | `[~]` | 70–75% | Impact Analyst/Remediation Planner/Reverification Planner 三个 RoleRuntime stage；typed inventory/impact；确定性下游失效闭包；授权/回滚/风险校验；Plan CAS；anti-downgrade HITL；token/cost/deadline；digest/freshness 复用；SQLite restart 与 GraphExecutor | remediation action 实际执行、产物新摘要回填、自动调用 F4V 复验、ApprovalStore 直连、跨 Store 原子协调、生产 calibration/live |
| LLM Eval/Judge | `[~]` | 70–75% | 分层 suite/run/trajectory 契约；Evaluation Memory View；匿名候选与顺序反转；Primary/Secondary/Adjudicator RoleRuntime；provider/model/group 独立性；agreement/kappa/bias/variance/ground-truth accuracy/Wilson CI；24 项规划/记忆/验收/修复/runtime 指标；flaky/missing/critical/regression 门禁；PDP/HITL upgrade/rollback；SQLite terminal report、restart 和 GraphExecutor | 真实仓库/领域/对抗/recovery/live 数据集，人工标注与 inter-rater 基线，生产 Judge/profile/prompt/model 校准，nightly scheduler、报告签名、trend/SLO、跨 Store atomic 与默认不可绕过 |
| LLM Observability | `[~]` | 82–87% | production Harness 强制 invocation evidence；GenAI span/metric；确定性采样；隐私受控 correlated log；provider/model/profile/prompt/view/route/fallback/token/cache/cost/latency/calibration 与 stage/memory pin 关联 | 真实 provider usage/cost 对账、全 production composition 跨进程 trace、cardinality 压测和 live evidence |
| Explainable UI / Operations | `[~]` | 75–80% | `phase4.operations.v1` display-safe contract；CLI/Web/TUI/ImGui 同源投影；Plan/Evidence、Memory、Invocation、五层 Assurance、Live/HITL；桌面真实截图与交互 | production Store revision assembler、ApprovalStore/PDP/identity executor、真实移动截图、production Live snapshot/replay |

v2 采用“**LLM Cognitive Plane + Deterministic Control Plane**”：LLM 深度参与分析、调查、综合、规划、记忆加工和专业解释；ACL、authority、policy、CAS、artifact/test/metric oracle 与最终 Arbiter 保持确定性。系统持久化结构化事实、证据、假设、备选方案、风险和决策理由，不持久化模型私有 chain-of-thought。

### 2.2 v1 工作包状态

| 工作包 | 状态 | 满足度 | 已有基础 | 未关闭的核心 DoD |
|---|---:|---:|---|---|
| WP4.0 Task Cognition | `[~]` | 78–84% | TaskIntake/Evidence/Understanding/Plan、生产只读 ToolBus Investigator、能力/预算/来源/摘要门、adaptive stop、DAG critic、SQLite Evidence/Plan/Cognition CAS、memory-aware workflow、bounded replan、默认 production adapter | ApprovalStore 直接事实源、跨 Store 原子提交、真实外部 investigator/provider calibration、remediation 自动回灌和 UI 治理动作 |
| WP4.1 Assurance Harness | `[~]` | 75% | AcceptanceContract、legacy verifier registry、Verification Planner、五专业角色与 Resolver、Manifest oracle、EvidenceLedger/resolution、oracle-strength arbiter、SQLite report/checkpoint、impact graph、remediation/selective reverify、restart/GraphExecutor | artifact/runtime/domain/security/metric 真实执行适配器、修复后自动复验、生产校准与 UI 未完成 |
| WP4.2 Durable Run | `[~]` | 87–89% | SQLite RunStore、状态机、CAS；P4-DRA 原子 checkpoint/graph cursor/event/effect/memory pin/interruption；摘要链 replay/time-travel；durable timer/restart；`DurableRunCoordinator`；effect CAS/reconcile；Run/Harness 跨库 saga journal、双 revision/digest pin、restart reconciliation/manual review | workflow adapters 尚未在每个 checkpoint 自动写入 saga、Approval/Memory 跨 Store 协调与生产 crash matrix仍未完成 |
| WP4.3 HITL/Policy | `[~]` | 81–85% | Approval schema/PDP/Store、CAS/TOCTOU/SoD/expiry/revocation；durable delegation grant；策略 quorum/required roles；edit supersession；escalation；authenticated action service；Web 移除默认 reviewer/fixed token 并在身份未配置时 fail closed；Harness resume | 真实 JWT/OIDC IdP attestation、session rotation、密码学/KMS 多签、delegation chain depth、跨 Approval/Run saga 及四端全部交互动作仍未完成 |
| WP4.4 Sandbox Runtime | `[~]` | 72–78% | Sandbox schema/provider/registry/policy、真实 Bubblewrap deny-all Process provider、workspace diff/digest、resource/timeout/process-group kill、opaque credential pipe/redaction、Harness durable receipt/reconcile | 细粒度 egress enforcing proxy、Container/Remote provider、逃逸/内核攻击 E2E 与默认生产 composition |
| WP4.5 OTel/SLO | `[~]` | 84–88% | correlation/privacy、W3C trace、deterministic sampling、span/metric/log、OTLP HTTP(S)、CA/hostname/client-cert、SQLite 三信号 spool/restart/backoff/capacity/dead-letter/retention、audit bridge、threshold/quantile SLO 与 multi-window error-budget/burn-rate fail-closed、loopback collector | 多 exporter lease/jitter、真实 Collector mTLS 证据、dashboard/alert backend、provider usage 对账 |
| WP4.6 Evaluation | `[~]` | 76–82% | 原有 Judge 能力；六层 versioned manifest/digest、双 reviewer label/kappa、flaky quarantine、immutable SQLite campaign/lease/report、签名防篡改、restart/trend | 生产分层数据与专家实际标注、真实 Judge calibration、部署 scheduler/KMS、长期 trend/SLO 和 live evidence |
| WP4.7 Live Certification | `[~]` | 76–81% | typed Environment/Profile/Matrix/Cell/Checkpoint/Report；角色独立、只读/blind/strong-oracle；R6L strict production bundle；WAL/FULL immutable attestation Store 与跨重启独立 verifier；Store-backed approval digest/SoD/scope/expiry binding；Ed25519 public verification；两阶段 ProductionLiveRunner（execute→AwaitingApproval→digest-bound approval→resume/sign）；显式 no-skip required gate；62/62 offline | 外部 scheduler、真实多 provider 与 IdP/MCP/A2A/Sandbox 全矩阵、实际 KMS signer、获批 production decision 和未过期 `executed=true` 报告仍未完成 |
| WP4.8 Distributed Control | `[~]` | 89–92% | SQLite WAL/FULL durable queue 与真实 rolling DDL；PostgreSQL/libpq shared queue、`SKIP LOCKED` 并发、DB-time lease、fencing/idempotency/quota、worker registry/heartbeat、leader election、backend reconnect、immediate crash/restart recovery，以及事务化真实 rolling DDL/失败回滚/租约接管；content-addressed ObjectStore/篡改检测；真实 fork worker-crash recovery；认证 TCP queue、原生 mTLS 双向认证、hostname/CA 校验与 TLS 1.2 minimum；mTLS RemoteObjectStore/二进制/tenant/digest 验证 | 真实多主机 mTLS deployment、PostgreSQL 复制/自动故障转移、S3/分布式复制 object backend、跨主机 network partition/clock-skew/leader kill chaos |
| WP4.9 Multi-layer Memory | `[~]` | 80–82% | SQLite v2 records/history/generation 与独立 digest-verified conflict store、六层 namespace、authority/lifecycle/ACL、Provider/View、9 profiles、AGENTS resolver、九阶段 LLM workflow、promotion/forget、v1 dual-read/restart；display-safe Inspector | hybrid lexical/vector index、index generation pin、Run/Invocation/Approval saga、cross-scope migration、Inspector→真实 Store 治理动作接入 |

WP4.9 使用尾部编号只是为了不重排已经冻结的 116 个任务 ID；它是 P0，并应在认知规划和五层验收垂直闭环之前达到可用状态。

## 3. WP4.0 实施前基线证据（保留用于差异追溯）

**可复用**：

- `include/agent/agent/user_input_types.hpp`：正文、注入块、控制动作和违规；
- `src/agent/user_input_preprocessor.cpp`：Tier A/B 输入处理；
- `src/node/agent_loop_node.cpp`：RAG、ToolBus、多轮 ReAct；
- `include/agent/agent/child_task.hpp`、`include/node/subflow_node.hpp`：有界子任务与子图；
- `src/a2a/orchestration.cpp`：远程 Agent 调度入口。

**判定**：这些能力支持调查和执行，但没有任务理解及计划领域模型；ReAct 中临时选择工具不能替代版本化专业计划。

## 4. WP4.1 实施前基线证据（保留用于差异追溯）

**可复用**：

- `include/agent/agent/verifier_types.hpp`：`ok/issues/suggested_action`；
- `src/agent/verifier_runner.cpp`：隔离的第二 LLM profile、timeout 和结构化输出；
- `include/agent/observability/audit.hpp`：trace、digest、redaction；
- Phase 2/3 CTest、A2A fixture、Faiss contract 和真实 Web 截图。

**关键限制**：现有 Verifier prompt 明确禁止调用工具或外部 API，只能检查用户任务与 draft answer；它只能成为 Assurance Harness 的一个语义 verifier，不能作为最终验收器。

## 5. WP4.2 实施前基线证据（保留用于差异追溯）

**可复用**：

- `SessionStore`：CAS revision、checkpoint ID、tool/child snapshots；
- `ToolEffectJournal`：Started/Completed/Committed 与 reconciliation；
- `MemoryStore`、Faiss、MCP registry：generation/manifest 恢复；
- Skill Workflow 和 ChildTask：checkpoint、retry/restart/resume。

**关键限制**：没有统一 RunState/RunStore；GraphExecutor 未持久化 node cursor、pending work、timer、interrupt、plan/acceptance revision 和 deterministic replay 元数据。

## 6. WP4.3 实施前基线证据（保留用于差异追溯）

**可复用**：

- `TaskControl`：cooperative cancel 与 deadline；
- Skill grant/policy、Tool hook 和 MCP fail-closed；
- Effect Journal 的 ManualReview 状态；
- AgentServer cancel API 与 SSE 状态更新。

**关键限制**：没有 ApprovalRequest/Decision schema、durable interruption、reviewer identity、expiry、delegation、参数编辑、双人复核和三端 UI pending queue。

## 7. WP4.4 实施前基线证据（保留用于差异追溯）

**可复用**：

- `FsSandboxConfig` 和 canonical path jail；
- Skill process 使用 `unshare + bwrap`、网络隔离、环境 allowlist、secret file、CPU/内存限制；
- Rich renderer 使用无 shell 子进程及 CPU/内存/文件/输出/超时限制。

**关键限制**：隔离逻辑分散在 Skill、FS 和 renderer；没有统一 SandboxProvider、workspace snapshot/diff、生命周期、container/remote provider、credential broker 和审计 manifest。

## 8. WP4.5 实施前基线证据（保留用于差异追溯）

**可复用**：

- Audit schema v2、JSONL/stderr/composite sinks；
- ExecutionEvent sequence 和 trace/task/session/run 关联；
- latency threshold、memory metrics、ChildTask token usage。

**关键限制**：没有 span parentage、OTLP exporter、标准 GenAI semantic convention、histogram、token/cost/cache/queue 指标、sampling、critical path、dashboard 和 SLO。

## 9. WP4.6 实施前基线证据（保留用于差异追溯）

**可复用**：默认构建中存在 3000 余个 CTest，其中 Agent Framework 已有 unit、contract、loopback、fault/recovery、UI 和 Phase 标签。

**关键限制**：没有独立 eval runner、dataset schema、trajectory recorder、模型/Prompt 版本矩阵、RAG/规划/验收质量指标、统计比较、flaky detection 或 judge calibration。

## 10. WP4.7 实施前基线证据（保留用于差异追溯）

`a2a_live_smoke` 和 `a2a_live_multi_agent` 提供 live 入口，但环境变量未设置时返回 0；普通绿色 CTest 不能证明实际执行。当前无强制 `executed=true` 证据、scheduled workflow、真实 IdP/MCP/LLM/renderer/sandbox 组合矩阵和环境 manifest。

## 11. WP4.8 实施前基线证据（保留用于差异追溯）

AgentServer 使用有界进程内 FIFO、worker threads、内存 active task map 和可选 SQLite SessionStore。A2A/ChildTask 支持远程执行，但没有 durable dispatch、任务所有权 lease、worker heartbeat、leader election、远程 artifact/evidence store、HA 和 rolling migration。

## 12. WP4.9 实施前基线证据（保留用于差异追溯）

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

## 15. 2026-08-09 首轮纵向实现增量证据

第 3–12 节刻意保留为实施前基线，不能再被解读为当前源码不存在相应类型。当前新增能力及证据如下：

| 能力域 | 当前实现 | 确定性证据 | 仍不可声称的能力 |
|---|---|---|---|
| 统一契约 | `contracts/contract` 及 planning/assurance/run/approval/memory/sandbox/telemetry/eval 类型 | `phase4_contracts` | 已发布 schema 的长期兼容性和线上迁移 |
| Durable Run/HITL | SQLite RunStore、CAS 状态机、event、interruption/token、timer；PDP | `phase4_durable_run`、`phase4_policy` | GraphExecutor 原子 replay、完整 ApprovalStore/UI |
| 多层记忆 | SQLite v2、六层 scope、Provider、9 种 View（含 Evaluation）、AGENTS resolver、治理/forget | `phase4_memory`、`phase4_memory_profiles` | hybrid index、v1 dual-read、冲突 HITL、Inspector/UI |
| 认知规划 | SQLite Evidence/Plan/Cognition checkpoint、RoleRuntime stage adapter、Intake→Strategy→Investigation→Synthesis→Boundary→Planner→Critic/Revision、GraphExecutor template | `phase4_cognition`、`phase4_planning_store`、3 个 `phase4_cognition_pipeline*` | 真实 ToolBus/外部 investigator/provider matrix、跨 Store 原子提交和默认强制接入 |
| 五层验收 | 只读 legacy registry；Verification Planner；五专业角色与 Resolver RoleRuntime；Manifest oracle；evidence closure/conflict resolution；强 oracle arbiter；SQLite checkpoint/report；restart 与 GraphExecutor | `phase4_assurance`、4 个 `phase4_assurance_workflow*` | 真实 Sandbox/ToolBus oracle adapters、生产 calibration/benchmark、默认强制接入 |
| 修复/重规划/复验 | `remediation` typed contracts、ImpactInventory/Graph、三角色 RoleRuntime、PlanStore CAS、PDP/HITL anti-downgrade、bounded budget、selective reverify、SQLite/GraphExecutor/restart | 4 个 `phase4_remediation_*` | 实际修复执行、新 artifact digest、自动 F4V 复验、ApprovalStore/跨 Store atomic、生产 calibration/live |
| Runtime/OTel | Sandbox spec/provider contract、workspace manifest；Span/Metric runtime 与隐私门 | `phase4_runtime_observability` | concrete provider、OTLP/SLO、跨进程传播 |
| Eval/Live/Distributed | immutable dataset、blind multi-Judge/校准/指标/upgrade gate、SQLite report/restart；certification contract；租约/fencing/DLQ 内存参考实现 | `phase4_eval`、4 个 `phase4_judge_*`、`phase4_live_distributed` | 生产数据集/Judge calibration/nightly/signed report、executed live matrix、远程持久队列、HA |
| 跨模块绑定 | Memory snapshot/view→Plan digest→Run checkpoint→五层 AcceptanceReport→restart terminal | `phase4_vertical` | 真实执行器副作用与远程依赖的端到端认证 |
| Role-based LLM Runtime | immutable role/profile/prompt/calibration、provider pool、schema gate、能力/Memory View/独立性约束、InvocationManifest SQLite CAS/recovery、AgentLoop 可选接入 | `phase4_llm_runtime_contracts`、`phase4_llm_runtime_routing`、`phase4_llm_runtime_store`、`phase4_llm_runtime_integration` | 尚未证明所有 Cognition/Memory/Assurance/Judge 路径不可绕过；无真实 provider/calibration/live matrix |

当前 `ctest -L phase4-offline` 为 **45/45 PASS**；Phase 3 `ctest -L phase3-offline` 为 **14/14 PASS**。首轮纵向证据见[首轮报告](./phase-4/acceptance-P4-F0R-F8-vertical-20260809.md)，P4-F2D 持久化增量见[增量报告](./phase-4/acceptance-P4-F2D-durable-stores-20260809.md)，F1L 证据边界见[Role Runtime 验收报告](./phase-4/acceptance-P4-V2-F1L-20260809.md)，F2C 证据边界见[多阶段认知规划验收报告](./phase-4/acceptance-P4-V2-F2C-20260809.md)，F3M 证据边界见[LLM 多层记忆验收报告](./phase-4/acceptance-P4-V2-F3M-20260810.md)，F4V 证据边界见[多角色专业验收报告](./phase-4/acceptance-P4-V2-F4V-20260810.md)，F5R 证据边界见[修复与选择性复验验收报告](./phase-4/acceptance-P4-V2-F5R-20260810.md)，F6E 证据边界见[Judge、评测与校准验收报告](./phase-4/acceptance-P4-V2-F6E-20260810.md)，F7L 证据边界见[真实角色与 Live 认证验收报告](./phase-4/acceptance-P4-V2-F7L-20260810.md)，F8U 证据边界见[解释性 UI 与运维呈现验收报告](./phase-4/acceptance-P4-V2-F8U-20260810.md)，R1I 证据边界见[Integrated Harness Runtime 验收报告](./phase-4/acceptance-P4-V2-R1I-20260811.md)。
