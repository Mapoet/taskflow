# Phase 4 需求—计划—执行—证据追溯矩阵

**用途**：防止需求、实现和验收脱节。状态只允许：`planned`、`in_progress`、`implemented`、`verified`、`accepted`、`partial`、`blocked`。

## 1. 主矩阵

| Requirement | Plan tasks | Status | Source/API | Tests | Runtime evidence | Acceptance |
|---|---|---|---|---|---|---|
| R4.0 证据驱动任务理解 | 4.0.1–4.0.9 | planned | 待实现 `planning/*` | `phase4-planning` | EvidenceBundle/Understanding digest | WP4.0 DoD |
| R4.0 可执行计划与重规划 | 4.0.10–4.0.16 | planned | 待实现 PlanStore/Workflow | DAG/CAS/replan/E2E | plan revision/diff/UI screenshot | WP4.0 DoD |
| R4.1 五层独立验收 | 4.1.1–4.1.16 | planned | 待实现 `assurance/*` | `phase4-assurance` | Findings/AcceptanceReport | WP4.1 DoD |
| R4.2 全运行持久化 | 4.2.1–4.2.12 | planned | 待实现 `run/*` | kill/restart/replay | Run checkpoint/recovery trace | WP4.2 DoD |
| R4.3 Durable HITL | 4.3.1–4.3.12 | planned | 待实现 approval/policy | `phase4-policy` | request/decision/audit/UI | WP4.3 DoD |
| R4.4 统一 Sandbox | 4.4.1–4.4.12 | planned | 待抽取 `sandbox/*` | escape/resource/E2E | SandboxManifest/workspace diff | WP4.4 DoD |
| R4.5 标准观测与 SLO | 4.5.1–4.5.12 | planned | 待实现 telemetry | OTLP loopback/context | trace/dashboard/SLO report | WP4.5 DoD |
| R4.6 系统评测 | 4.6.1–4.6.13 | planned | 待实现 `eval/*` | small/nightly datasets | comparison report | WP4.6 DoD |
| R4.7 Live 认证 | 4.7.1–4.7.11 | planned | CI/runner 待实现 | scheduled live matrix | executed attestation/cert | WP4.7 DoD |
| R4.8 分布式控制面 | 4.8.1–4.8.12 | planned | remote stores/queue 待实现 | chaos/load/HA | lease/failover report | WP4.8 DoD |
| R4.9 五层 scope 与 Provider | 4.9.1–4.9.10 | planned | 待实现 `memory_v2/*` provider/schema | namespace/ACL/provider/adapter | MemoryRecord/provider generation | WP4.9 DoD |
| R4.9 Scope-first 检索与动态 View | 4.9.11–4.9.16 | planned | 待实现 index/view/resolver/assembly v2 | retrieval/conflict/view/recovery | ViewManifest/snapshot/view digest | WP4.9 DoD |
| R4.9 Consolidation 与治理 | 4.9.17–4.9.19 | planned | 待实现 promotion/governance/inspector | promotion/forget/RBAC/UI | candidate/decision/audit/screenshots | WP4.9 DoD |
| R4.9 迁移与垂直认证 | 4.9.20 | planned | v1 adapter/dual-read 待实现 | `phase4-memory` E2E/eval | cross-scope/restart/report | WP4.9 DoD |

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
