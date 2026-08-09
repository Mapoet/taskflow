# Phase 4 需求—计划—执行—证据追溯矩阵

**用途**：防止需求、实现和验收脱节。状态只允许：`planned`、`in_progress`、`implemented`、`verified`、`accepted`、`partial`、`blocked`。

## 1. 主矩阵

| Requirement | Plan tasks | Status | Source/API | Tests | Runtime evidence | Acceptance |
|---|---|---|---|---|---|---|
| R4.0 证据驱动任务理解 | 4.0.1–4.0.9 | implemented | `planning/types,evidence_store,sqlite_planning_store,cognition_workflow` | `phase4_cognition`、`phase4_planning_store` | durable EvidenceBundle/Understanding/View digest | real adapters pending |
| R4.0 可执行计划与重规划 | 4.0.10–4.0.16 | partial | `planning/plan_store,sqlite_planning_store,plan_validator` | DAG/CAS/cycle/restart/parent-digest negative | durable plan revision digest | executor replan/UI pending |
| R4.1 五层独立验收 | 4.1.1–4.1.16 | implemented | `assurance/evidence,verifier,registry,arbiter,harness` | `phase4_assurance` | Findings/AcceptanceReport | real verifier adapters/store pending |
| R4.2 全运行持久化 | 4.2.1–4.2.12 | implemented | `run/state_machine,store` + SQLite | `phase4_durable_run` | restart/CAS/timer/token | executor replay pending |
| R4.3 Durable HITL | 4.3.1–4.3.12 | partial | `approval/types,policy,store`; Run interruption | `phase4_policy`、`phase4_approval_store`、durable run | PDP/request/decision/token/restart | delegation/multisig/edit-flow/UI pending |
| R4.4 统一 Sandbox | 4.4.1–4.4.12 | partial | `sandbox/types,provider,workspace` | `phase4_runtime_observability` | deterministic workspace/spec | concrete providers pending |
| R4.5 标准观测与 SLO | 4.5.1–4.5.12 | partial | `telemetry/types,runtime` | `phase4_runtime_observability` | span/metric/privacy gate | OTLP/SLO pending |
| R4.6 系统评测 | 4.6.1–4.6.13 | partial | `eval/types,runner` | `phase4_eval` | deterministic run/comparison | datasets/judge/nightly pending |
| R4.7 Live 认证 | 4.7.1–4.7.11 | partial | `live/certification` | `phase4_live_distributed` | skip-as-fail/expiry | executed live matrix pending |
| R4.8 分布式控制面 | 4.8.1–4.8.12 | partial | `distributed/durable_queue` reference semantics | fencing/reclaim/DLQ | lease tokens | remote durability/HA pending |
| R4.9 五层 scope 与 Provider | 4.9.1–4.9.10 | implemented | `memory_v2/types,store,provider,project_instruction_provider` | `phase4_memory*` | ACL/provider generation | upstream adapters pending |
| R4.9 Scope-first 检索与动态 View | 4.9.11–4.9.16 | implemented | `memory_v2/view_engine,view_profiles` | isolation/budget/restart/digest | ViewManifest/snapshot | hybrid retrieval/atomic Run pin pending |
| R4.9 Consolidation 与治理 | 4.9.17–4.9.19 | partial | `memory_v2/governance` | promotion/forget/sink | decision/tombstone | consolidator/Inspector/UI pending |
| R4.9 迁移与垂直认证 | 4.9.20 | partial | v2 isolated path；Memory→Plan→Run→Acceptance 纵向绑定 | `phase4_vertical` + Phase 3/4 offline regression | 27 tests PASS；restart terminal binding | dual-read/real executor/live E2E pending |

## 2. 行更新规则

1. `implemented` 需要 source/API 和模块测试，但不代表任务完成。
2. `verified` 需要所有适用层的 evidence；缺少真实依赖只能 partial/blocked。
3. `accepted` 必须填写 AcceptanceReport 路径、digest、plan revision 和适用的 MemorySnapshot/View digest。
4. 每个 source/test/evidence 应使用仓库相对路径、CTest 名或可长期访问的 artifact ID。
5. 一个 requirement 拆分后新增子行，不删除历史行；被替代行标记 superseded 和 decision ID。
6. 任何 mandatory criterion 的删除或降级必须关联 HITL decision。
7. 涉及 Memory 的 requirement 必须同时填写 source authority、scope、provider/index generation、ViewManifest 和 promotion/forget decision；只有 Prompt 或向量命中不能作为完成证据。

## 3. 单任务扩展模板

| Requirement | Plan node/revision | Commit/files | Test layers | Evidence IDs | Findings | Decision |
|---|---|---|---|---|---|---|
| `R...` | `4.x.y / plan-r...` | 待填 | F/M/I/S/Metric | 待填 | 待填 | pending |
