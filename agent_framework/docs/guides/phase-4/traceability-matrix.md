# Phase 4 需求—计划—执行—证据追溯矩阵

**用途**：防止需求、实现和验收脱节。状态只允许：`planned`、`in_progress`、`implemented`、`verified`、`accepted`、`partial`、`blocked`。

**当前计划版本**：[`phase4-v2-plan-r1`](../phase-4-plan-v2.md) + [`phase4-v2-residual-r1`](../phase-4-residual-closure-plan.md)；原 R4.0–R4.9 行继续保留，v2 扩展与 closure requirement 不得覆盖已有实现证据。

## 1. 主矩阵

| Requirement | Plan tasks | Status | Source/API | Tests | Runtime evidence | Acceptance |
|---|---|---|---|---|---|---|
| R4.0 证据驱动任务理解 | 4.0.1–4.0.9 | implemented | `planning/types,evidence_store,sqlite_planning_store,cognition_workflow` | `phase4_cognition`、`phase4_planning_store` | durable EvidenceBundle/Understanding/View digest | real adapters pending |
| R4.0 可执行计划与重规划 | 4.0.10–4.0.16 | partial | `planning/plan_store,sqlite_planning_store,plan_validator` | DAG/CAS/cycle/restart/parent-digest negative | durable plan revision digest | executor replan/UI pending |
| R4.1 五层独立验收 | 4.1.1–4.1.16 | implemented | `assurance/evidence,verifier,registry,arbiter,harness` | `phase4_assurance` | Findings/AcceptanceReport | real verifier adapters/store pending |
| R4.2 全运行持久化 | 4.2.1–4.2.12 + P4-CSAC0–9 | partial | `run/state_machine,store`；`harness/{run_binding_saga,cross_store_coordination}` | `phase4_durable_run`、`phase4_run_harness_saga`、`phase4_cross_store_coordination` | restart/CAS/timer/token；cross-Store receipt/manual-review | command participant、cross-process fencing/HA pending |
| R4.3 Durable HITL | 4.3.1–4.3.12 | partial | `approval/types,policy,store`; Run interruption | `phase4_policy`、`phase4_approval_store`、durable run | PDP/request/decision/token/restart | delegation/multisig/edit-flow/UI pending |
| R4.4 统一 Sandbox | 4.4.1–4.4.12 | partial | `sandbox/types,provider,workspace` | `phase4_runtime_observability` | deterministic workspace/spec | concrete providers pending |
| R4.5 标准观测与 SLO | 4.5.1–4.5.12 | partial | `telemetry/types,runtime` | `phase4_runtime_observability` | span/metric/privacy gate | OTLP/SLO pending |
| R4.6 系统评测 | 4.6.1–4.6.13 | partial | `eval/{types,runner,judge_types,judge_workflow,sqlite_judge_store,judge_graph_template}` | `phase4_eval`、4 个 `phase4_judge_*` | blind assignments、InvocationManifest、calibration/metric/upgrade decision、SQLite report | production datasets/calibration/nightly/signed report pending |
| R4.7 Live 认证 | 4.7.1–4.7.11 | partial | `live/{certification,role_certification*,sqlite_role_certification_store}`；RoleRuntime adapter；GraphExecutor template | `phase4_live_distributed`、`phase4-live-v2` 5/5、显式 `phase4_live_required` no-skip artifact gate | strict role/profile/provider/model/prompt binding、skip-as-fail、failure recovery、CAS restart、签名/审批/expiry | 外部 production runner、真实凭据/全矩阵和批准 `executed=true` 报告 pending |
| R4.8 分布式控制面 | 4.8.1–4.8.12 | partial | `distributed/durable_queue` reference semantics | fencing/reclaim/DLQ | lease tokens | remote durability/HA pending |
| R4.9 五层 scope 与 Provider | 4.9.1–4.9.10 | implemented | `memory_v2/types,store,provider,project_instruction_provider` | `phase4_memory*` | ACL/provider generation | upstream adapters pending |
| R4.9 Scope-first 检索与动态 View | 4.9.11–4.9.16 | implemented | `memory_v2/view_engine,view_profiles` | isolation/budget/restart/digest | ViewManifest/snapshot | hybrid retrieval/atomic Run pin pending |
| R4.9 Consolidation 与治理 | 4.9.17–4.9.19 | partial | `memory_v2/governance` | promotion/forget/sink | decision/tombstone | consolidator/Inspector/UI pending |
| R4.9 迁移与垂直认证 | 4.9.20 | partial | v2 isolated path；Memory→Plan→Run→Acceptance→Remediation 纵向绑定 | `phase4_vertical` + Phase 3/4 offline regression | 56 tests PASS；restart terminal binding | dual-read/real executor/live E2E pending |

## 2. Phase 4 v2 扩展矩阵

| Requirement | Plan tasks | Status | Current source/API | Current tests/evidence | Mandatory gap / acceptance |
|---|---|---|---|---|---|
| `R4V2-00` 双基线与治理 | `P4-V2-F0.1–F0.5` | implemented | `phase-4-plan-v2.md`、status、decision、ledger | 文档 ID/链接/口径检查 | 文档基线验收后保持；代码完成度不得由本行推断 |
| `R4V2-01` Role-based LLM Runtime | `P4-V2-F1L.1–F1L.12` | partial | `llm_runtime/{types,store,registry,router,structured_output,runtime}`；`LLMClient` request-scoped profile/usage；AgentLoop optional adapter | 4 个 `phase4_llm_runtime_*` 离线门禁；contract/tamper、routing negative、SQLite restart/CAS、fallback/repair/telemetry/audit/idempotency | 离线核心 implemented；独立 StructuredOutputPolicy、overlay/default revision、GraphExecutor 强制接入、timeout/live provider/calibration matrix pending，故 requirement 不升级为 verified |
| `R4V2-02` 多阶段认知规划 | `P4-V2-F2C.1–F2C.10` | partial | `planning/{cognition_pipeline,sqlite_cognition_store,cognition_graph_template,cognition_workflow,evidence_store,plan_store,plan_validator}`；`RoleRuntimeCognitionModel` | `phase4_cognition_pipeline_contracts`、`phase4_cognition_pipeline`、`phase4_cognition_pipeline_integration`：strict contract/CAS、负例、RoleRuntime、独立 Critic、HITL revision、process-death recovery、GraphExecutor；验收报告 `acceptance-P4-V2-F2C-20260809.md` | 离线核心 implemented；真实 investigator/provider/calibration、adaptive stop、跨 Store 原子提交、ApprovalStore 直连、默认强制门禁和 live/SLO pending，故 requirement 不升级为 verified |
| `R4V2-03` LLM 多层记忆 | `P4-V2-F3M.1–F3M.9` | partial | `memory_v2/{types,store,provider,view_engine,view_profiles,governance,workflows/*}`；`RoleRuntimeMemoryModel`；shared `internal/sqlite_utils`；legacy dual-read adapter | `phase4_memory_workflow_{contracts,negative,integration}`、`phase4_memory_workflow`：strict/CAS、9 RoleRuntime routes、scope/ACL/provider leak、poisoning、candidate-only、HITL promotion/forget、v1 dual-read、restart、GraphExecutor；报告 `acceptance-P4-V2-F3M-20260810.md` | 离线核心 implemented；真实 profiles/provider calibration、可执行 hybrid index、跨 Store atomic/ApprovalStore、cross-scope migration、默认强制、Live/HA/Inspector pending，故不升级为 verified |
| `R4V2-04` 多角色专业验收 | `P4-V2-F4V + P4-PAO0–PAO9` | partial | `assurance/{professional_workflow,production_oracles,evidence,arbiter}`；repository/file、Bubblewrap command、domain/security SPI；production mandatory coverage | 原 F4V 四测试 + `phase4_production_oracles`；真实路径/非空/digest、Sandbox receipt、缺 source fail-closed、mandatory strength | 本地 production Oracle 控制面 implemented；部署级完整规则、真实 domain/security adapter、双 verifier/calibration benchmark、跨 Store atomic 与 external Live pending |
| `R4V2-05` 修复与选择性复验 | `P4-V2-F5R.1–F5R.7` | partial | `remediation/{remediation_workflow,sqlite_remediation_store,remediation_graph_template}`；`RoleRuntimeRemediationModel`；`planning::PlanStore/PlanValidator`；PDP/approval decision validator | `phase4_remediation_{contracts,workflow,negative,restart}`：strict/CAS、Impact closure、授权/回滚、anti-downgrade、token/cost、重复 finding/proposal loop、selective rerun、RoleRuntime、GraphExecutor、commit/checkpoint crash recovery；报告 `acceptance-P4-V2-F5R-20260810.md` | 离线控制面 implemented；实际 remediation action executor、新 artifact digest 回填、自动 F4V rerun、ApprovalStore 直连、跨 Store atomic、生产 calibration/live pending，故不升级为 verified |
| `R4V2-06` Judge 与校准 | `P4-V2-F6E.1–F6E.9` | partial | `eval/{judge_workflow,sqlite_judge_store,judge_graph_template}`；`RoleRuntimeJudgeModel`；Evaluation Memory View；PDP/approval validator | `phase4_judge_{contracts,workflow,negative,integration}`：strict/tamper/CAS、blind leakage、双 Judge/仲裁、独立 provider/model/group、agreement/kappa/bias/variance/GT accuracy/Wilson CI、24 项指标、flaky/missing/critical/regression、upgrade/rollback、RoleRuntime、SQLite restart/atomic report、GraphExecutor；报告 `acceptance-P4-V2-F6E-20260810.md` | 离线控制面 implemented；真实仓库/领域/对抗/recovery/live 数据集、人工标注/inter-rater、生产 Judge calibration、nightly scheduler、signed report、trend/SLO、跨 Store atomic/default gate pending，故不升级为 verified |
| `R4V2-07` LLM 角色 Live 认证 | `P4-V2-F7L.1–F7L.7` | partial | `live/{role_certification,role_certification_graph_template}`；InMemory/SQLite store；`RoleRuntimeLiveCellExecutor`；HMAC signed-report gate | `phase4-live-v2` 5/5：contract/workflow/negative/restart/真实 RoleRuntime manifest integration；`AGENT_ENABLE_PHASE4_LIVE_CERTIFICATION` 缺配置 exit 2 | 离线控制面 accepted；真实 production role/provider/profile/prompt/calibration matrix、scheduled runner、KMS/IdP/MCP/A2A/Sandbox evidence 和未过期 `executed=true` report 未执行，不能 verified |
| `R4V2-08` 解释性 UI | `P4-V2-F8U.1–F8U.6` | partial | `ui/{phase4_operations,presentation_model,ui_manager}`；CLI/Web/TUI/ImGui adapters；Web snapshot/HITL routes | `phase4_operations_ui`、rich UI 6/6、`phase4-offline` 42/42、`phase3-offline` 14/14；Web/TUI/ImGui 真实截图；报告 `acceptance-P4-V2-F8U-20260810.md` | canonical/display-safe 同源呈现 accepted；production Store assembler、ApprovalStore executor、移动端真实截图和 production Live snapshot pending，故不升级 verified |

## 3. Residual Closure 矩阵

| Requirement | Plan tasks | Status | Direct evidence required | Current finding |
|---|---|---|---|---|
| `R4V2-RC-00` 基线治理 | `R0` | accepted | residual plan、status、trace、decision、ledger 一致 | 28 个本地链接、0 失效；26 个唯一 closure ID、0 重复 task definition；`git diff --check` PASS |
| `R4V2-RC-01` Integrated Harness | `P4-V2-R1I.1–R1I.6` | partial | `harness/{types,store,runtime}`；SQLite saga/event/outbox；typed ports；revision pin；completion gate；Store-backed Operations projection；`phase4_harness_*` 3/3 | R1I runtime core accepted；真实 Cognition/Memory/Assurance/Remediation/Judge/Sandbox/Approval adapters 和默认生产入口由 R2X/R3A 继续关闭 |
| `R4V2-RC-02` 修复执行与复验 | `P4-V2-R2X.1–R2X.6` | partial | jailed executor、SQLite durable journal/replay、rollback、真实 manifest/digest、filesystem oracle、Harness reject→repair→reverify→complete 与 failed repair→bounded ManualReview；47/47 | 具体 Sandbox command oracle 由 R4O 关闭 |
| `R4V2-RC-03` Approval/Memory/UI | `P4-V2-R3A.1–R3A.6` | partial | accountable reviewer/SoD/delegation/双人复核/CAS/reconcile；Approval-bound promotion/correction/forget；Harness resume；Harness+Approval+Memory revision join、redacted snapshot/replay；Web durable action；CLI/TUI/ImGui/Web 同一 SQLite snapshot 跨进程回放及真实截图；51/51 | UI 同源边界已关闭；默认生产 Harness composition 仍待关闭 |
| `R4V2-RC-04` Sandbox/OTLP/SLO | `R4O` | partial | `BubblewrapSandboxProvider`、opaque credential pipe/redaction、workspace diff、W3C trace、OTLP HTTP batch + SQLite spool、Audit bridge、SLO fail-closed、Harness SQLite receipt/reconcile；真实 sandbox 与跨进程 collector targeted 4/4 | deny-all profile、durable confirmed-receipt replay 与 telemetry restart replay 已验证；细粒度 egress proxy、孤儿 effect 自动判定、真实 Collector TLS、默认 Harness release 装配仍待关闭 |
| `R4V2-RC-05` Dataset/Calibration/Nightly | `R5E` | partial | 真实版本化数据集、人工基线、nightly signed trend report | 六层 manifest/人工标签 schema/kappa/flaky/immutable campaign/lease/signed trend 与重启已验证；真实数据、专家标注、生产校准/scheduler/KMS pending |
| `R4V2-RC-06` Production Live | `R6L-R0–R6 / phase4-r6l-production-live-r1` | blocked / executing | 获批 Environment/Matrix；真实 mandatory role/dependency cells；durable independent attestation；accountable ApprovalStore decision；非对称/KMS signature；未过期、无 blocker 的 `executed=true` 报告 | R0/R1/R3/R4 与 runner：strict bundle、SQLite immutable attestation/restart verifier、Store-backed approval binding、Ed25519 required gate、两阶段 execute/approve/resume/sign runner，targeted 5/5、offline 62/62；R2/R5/R6 仍缺真实多 provider/IdP/MCP/A2A/Sandbox、scheduler、实际 KMS signer/approval，禁止签发认证 |
| `R4V2-RC-07` Distributed/HA | `R7D` | partial | remote durable queue/shared store/multi-process chaos | SQLite 多连接 queue/rolling DDL；PostgreSQL shared queue/quota/worker/leader、DB-time fencing、`SKIP LOCKED`、fork crash、backend reconnect、immediate DB restart recovery，以及事务化 rolling DDL/失败回滚/租约接管；content-addressed ObjectStore；服务端时间裁决；原生 mTLS/CA/hostname/TLS downgrade；RemoteObjectStore 二进制/tenant/篡改验证已完成。真实多主机 deployment、PostgreSQL 复制/自动故障转移、复制 object backend 与 network partition/clock-skew chaos pending |
| `R4V2-GPW` Production golden-path wiring | `P4-GPW0–GPW10` | verified-local | `graph_executor` production closure binding；`harness/task_closure` runtime/progress；AgentServer/A2A/ChildTask；五 demo LiveRuntime profile | production missing-binding 与 false-completion negatives；Golden A/B/C；targeted 4/4；五 demo build；Phase4 73/73、Phase3 22/22。外部 R6L bundle/IdP/KMS/provider/multi-node 仍 pending |
| `R4V2-CTR` Conversation/Turn runtime | `P4-CTR0–CTR10` | verified-local | `conversation/{types,store,turn_state_machine,conversation_engine,context_projection,graph_turn_adapter,production_bridge}`；五入口 bootstrap | typed negative、SQLite parent/digest/CAS/restart、100 Turn、mandatory compact protection、profile upgrade、Turn cannot verify；Phase4 74/74、Phase3 22/22 | Tool Lifecycle 与全部实时入口完全切换仍 pending；R6L external evidence 独立开放 |

## 4. 行更新规则

1. `implemented` 需要 source/API 和模块测试，但不代表任务完成。
2. `verified` 需要所有适用层的 evidence；缺少真实依赖只能 partial/blocked。
3. `accepted` 必须填写 AcceptanceReport 路径、digest、plan revision 和适用的 MemorySnapshot/View digest。
4. 每个 source/test/evidence 应使用仓库相对路径、CTest 名或可长期访问的 artifact ID。
5. 一个 requirement 拆分后新增子行，不删除历史行；被替代行标记 superseded 和 decision ID。
6. 任何 mandatory criterion 的删除或降级必须关联 HITL decision。
7. 涉及 Memory 的 requirement 必须同时填写 source authority、scope、provider/index generation、ViewManifest 和 promotion/forget decision；只有 Prompt 或向量命中不能作为完成证据。

## 5. 单任务扩展模板

| Requirement | Plan node/revision | Commit/files | Test layers | Evidence IDs | Findings | Decision |
|---|---|---|---|---|---|---|
| `R...` | `4.x.y / plan-r...` | 待填 | F/M/I/S/Metric | 待填 | 待填 | pending |
