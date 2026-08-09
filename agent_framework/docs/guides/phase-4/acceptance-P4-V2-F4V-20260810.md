# P4-V2-F4V 多角色专业验收与五层验证验收报告

**日期**：2026-08-10
**计划**：`phase4-v2-plan-r1 / P4-V2-F4V.1–F4V.10`
**授权**：用户明确批准实施 P4-V2-F4V
**结论**：离线核心 `accepted`；完整 F4V `partial`，不得声称 production complete

## 1. 实施结果

| Plan node | 已实现结果 | 状态 |
|---|---|---|
| `F4V.1` | Verification Planner RoleRuntime stage 从 task context、AcceptanceContract、ArtifactManifest digest 和 Verification View 生成 role assignment；确定性 gate 保证全 criterion 覆盖、证据不扩张及只读能力 | offline accepted |
| `F4V.2` | `ManifestEvidenceOracle` 将 tenant/task/digest-bound observation manifest 转换为 deterministic/real-system/authoritative/static-analysis evidence；执行者/模型自述自动降为不独立的 uncalibrated claim | offline accepted / real adapters pending |
| `F4V.3` | Code Verifier 使用独立 RoleRuntime profile/view/capability 形成引用闭包内 finding | offline accepted |
| `F4V.4` | Architecture Verifier 检查 module/contract/dependency criterion，和 Planner/先前 verifier 使用不同 independence group | offline accepted |
| `F4V.5` | Domain Verifier 形成领域/标准/运行证据解释；只能引用已登记 evidence ID | offline accepted |
| `F4V.6` | Security Verifier 使用只读 capability intersection；未知、跨租户或未授权 evidence/capability fail closed | offline accepted |
| `F4V.7` | Completeness Verifier 检查系统生成物与需求覆盖；既有测试通过但 mandatory artifact 缺失时不能 Accepted | offline accepted |
| `F4V.8` | Planner/Executor/Verifier/Resolver independence enforcement；模型、provider 多样性策略可配置；执行者自证不能满足 mandatory evidence | offline accepted |
| `F4V.9` | 确定性 conflict detector 按 criterion/strength 识别 PASS/FAIL；Resolver 只追加 advisory analysis，不能改变 resolved flag；同强度冲突进入 ManualReview | offline accepted |
| `F4V.10` | 强 oracle 优先 Arbiter；InMemory/SQLite AssuranceStore；CAS、WAL/FULL/private permission、attempt/restart、terminal checkpoint+AcceptanceReport 单事务提交、GraphExecutor template | offline accepted / live pending |

所有七个语义阶段均可通过 `RoleRuntimeAssuranceModel` 调用 pinned profile/prompt/calibration revision。LLM 只拥有专业解释与 finding 建议权，不能执行副作用、改变 AcceptanceContract、伪造 evidence、降低 mandatory criterion 或直接决定持久终态。

## 2. 确定性安全边界

```text
Task/Plan/AcceptanceContract + pinned Verification View
  → LLM Verification Planner
  → deterministic coverage/capability/evidence-requirement gate
  → digest-bound Artifact/Runtime observation oracles
  → five independent professional LLM verifiers
  → deterministic evidence closure + independence enforcement
  → LLM Evidence Resolver advisory analysis
  → deterministic strength/conflict resolution
  → deterministic Acceptance Arbiter
  → atomic terminal checkpoint + AcceptanceReport
```

- oracle priority保持：deterministic → real system → authoritative → static analysis → calibrated model → uncalibrated claim；低等级 PASS 不能覆盖高等级 FAIL；
- ArtifactManifest 必须具有有效 canonical digest，并与 tenant/task 精确绑定；未知字段、未知 criterion、非法 outcome/strength 和错误 digest 均拒绝；
- Verification Planner 不能授予 `repo_write` 等副作用能力，也不能扩张 AcceptanceContract 的 evidence requirements；
- RoleRuntime 实际 capability 为 immutable role binding 与 VerificationPlan allowance 的交集；
- verifier 输出必须为每个 assigned criterion 提供唯一 finding，且每个 evidence ID 都属于当前 checkpoint evidence closure；
- Planner、Executor 和已执行 verifier 的 independence group 被后续角色禁止；provider/model diversity 可按风险策略启用；
- Resolver 的文字建议不能改变强度比较、冲突 resolved 状态或 Arbiter 结果；
- 进程死亡前先提交 stage attempt boundary，恢复后不重复已完成的 oracle/role stage。

## 3. 持久化和兼容

- 新增 `agent.verification_plan/v1`、`agent.evidence_resolution/v1`、`agent.assurance_checkpoint/v1` strict typed contracts；
- SQLite `assurance_checkpoints` 与 `acceptance_reports` 使用公共 `agent/internal/sqlite_utils.hpp`；
- terminal checkpoint 与 report 在同一 SQLite transaction 中提交，report digest 回绑 checkpoint；
- Store load 时重新验证 canonical digest、revision 和 tenant/workflow key；
- legacy `AssuranceHarness`、`VerifierRegistry`、`VerificationContext` 和 `AcceptanceArbiter` 保持兼容；新工作流与 GraphExecutor template 需要显式启用，可回滚到 legacy path。

## 4. 五层验证证据

| 层 | 证据 | 结果 |
|---|---|---|
| 功能 | Planner→oracle→Code/Architecture/Domain/Security/Completeness→Resolver→Arbiter→report | PASS |
| 模块 | strict contract/unknown/digest、InMemory/SQLite CAS/reopen/private permission、atomic report、read-only capability intersection | PASS |
| 集成 | 七个真实 RoleRuntime role routes、approved calibration、Verification View、Invocation Store、process-death restart、GraphExecutor | PASS |
| 综合 | deterministic FAIL vs LLM PASS、测试通过但产物缺失、未知 evidence、write capability、Planner 自验收、Executor claim、同强度冲突 | PASS |
| 指标/发布 | false accept on strong oracle failure=0；unauthorized evidence/capability accepted=0；CI Phase 4 minimum 24→28；Phase 4/3 回归 | OFFLINE PASS / LIVE PENDING |

实际命令与结果：

```text
ctest --test-dir build -L phase4-assurance-v2 --output-on-failure  # 4/4 PASS
ctest --test-dir build -L phase4-offline --output-on-failure       # 28/28 PASS
ctest --test-dir build -L phase3-offline --output-on-failure       # 14/14 PASS
```

## 5. 未关闭项与边界

1. `ManifestEvidenceOracle` 只验证和投影可信 observation manifest；尚无通过统一 Sandbox/ToolBus 执行 repo inspection、build/test、runtime fault、metric、domain/security 工具的生产 adapter；
2. 尚无真实任务集校准的 Code/Architecture/Domain/Security/Completeness/Resolver profiles、prompts、providers 和 finding precision/recall/false accept/false reject benchmark；
3. 关键角色目前强制 independence group 不同，provider/model diversity 为策略选项；尚无基于风险和校准数据的双 verifier/交叉模型自动策略；
4. AssuranceStore 内 terminal checkpoint/report 原子，但与 Run/Plan/Memory/LLM Invocation/Approval Store 不构成跨 Store transaction；通过 digest 绑定，不宣称全局 exactly-once；
5. Verification View 恢复要求 snapshot/view digest 可重现；未持久化完整 provider snapshot document，provider generation 变化将安全进入 ManualReview；
6. F4V 只形成 durable finding/report；finding→plan/artifact impact graph、bounded remediation/replan、criterion anti-downgrade 和 selective re-verification 属于 F5R；
7. GraphExecutor template 需要显式注册，尚未成为所有 agent execution 的不可绕过 terminal gate；legacy Harness 仍可独立调用；
8. cancellation 为 stage/provider 前后协作检查；SQLite 为单节点，没有跨进程 lease、HA、chaos、SLO 或 live provider failure matrix；
9. 尚无 Assurance Console CLI/Web/TUI、finding/evidence/conflict/manifest inspector 和真实运行截图；
10. 尚未接入 F6E blind/multi-judge 校准与 F7L scheduled no-skip live certification。

因此 `R4V2-04` 保持 `partial`，成熟度估计 **75–80%**。本批证明多角色 LLM 专业判断与确定性证据/裁决平面的离线闭环，不证明真实工具采集、模型专业准确性、生产强制接入、Live/HA 或 UI 运维能力已经达标。
