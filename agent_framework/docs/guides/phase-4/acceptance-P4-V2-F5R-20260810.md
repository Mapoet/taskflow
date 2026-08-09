# P4-V2-F5R 离线核心验收报告

**批次**：`P4-V2-F5R-OFFLINE-CORE-20260810`
**计划**：`P4-V2-F5R.1–F5R.7 / phase4-v2-plan-r1`
**结论**：离线控制面 accepted；`R4V2-05` 保持 partial

## 已实现范围

- `ImpactInventory` 显式绑定 requirement、criterion、plan node、artifact、evidence、memory record 和 verifier role；LLM 只能引用清单内对象。
- Impact Analyst 的建议与确定性闭包合并；非 PASS finding、artifact 依赖下游、关联 evidence/memory/verifier 均进入失效范围，模型遗漏不能缩小闭包。
- Remediation Planner 输出 finding 覆盖、最小 action、能力、side effect、rollback、risk 和 criterion change；未知 finding、越界 artifact/node、未授权能力、缺 rollback 和高风险免审批均 fail closed。
- 修复节点形成 `ExecutionPlan` 新 revision，绑定原 plan canonical digest，并通过 `PlanValidator` 与 `PlanStore::compare_exchange` 提交。
- 任意 criterion mandatory/threshold 变化及 governed action 生成 approval request digest；无绑定 decision 时持久停在 `AwaitingApproval`。
- Reverification Planner 的选择经确定性规则扩充：污染 evidence 一律失效；strong oracle 强制重跑；旧 evidence 只有 digest 格式和 freshness 同时有效且未受影响时才能复用。
- stage attempt、InvocationManifest 摘要、token/cost、产物、approval、proposed/committed plan 和 selective-reverification plan 进入 SQLite CAS checkpoint；计划已提交而 checkpoint 未推进时可幂等恢复。
- 三个语义 stage 均有生产 `RoleRuntimeRemediationModel` 适配器；同一 durable workflow 可由 `RemediationGraphTemplate` 接入 GraphExecutor。

## 验证证据

| 层级 | 证据 |
|---|---|
| 契约/模块 | `phase4_remediation_contracts`：typed contract digest/tamper、unknown field、InMemory/SQLite CAS、重开恢复 |
| 功能 | `phase4_remediation_workflow`：finding mapping、下游 impact closure、plan revision、污染证据拒绝、fresh evidence reuse |
| 策略负例 | `phase4_remediation_negative`：criterion anti-downgrade 等待审批、审批后新 contract digest、未授权 capability、token budget、重复 finding、重复 proposal loop→manual review |
| 集成/恢复 | `phase4_remediation_restart`：三个真实 RoleRuntime route/Memory Replan View、计划提交后故障、SQLite 重启、CAS 幂等、不重复提交 |
| 综合 | `phase4-remediation-v2` 4/4 PASS；`phase4-offline` 32/32 PASS；`phase3-offline` 14/14 PASS |

## 证据边界与残余风险

本批次的 terminal 是 `ReadyForExecution`，不是“修复完成”或“重新验收通过”。尚未关闭：

- action executor 未实际修改 workspace/远程系统，也没有生成并回填新 artifact digest；
- ReverificationPlan 尚未自动实例化 F4V 新 VerificationPlan/Oracle run/AcceptanceReport；
- Approval 目前通过 decision validator 边界接入，尚未由 workflow 直接创建/读取 `ApprovalStore` request/decision；
- PlanStore、RemediationStore、ApprovalStore、InvocationStore 和未来 ArtifactStore 之间没有单事务提交；
- 当前通过的是离线 fake/calibrated fixture，不是生产 provider/model/profile calibration 或 live SLO。

因此 F5R.1–F5R.7 的离线控制面可接受，但 Phase 4 的生产 remediation 闭环不得标记 complete。
