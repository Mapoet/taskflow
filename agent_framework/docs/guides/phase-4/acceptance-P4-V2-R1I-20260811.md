# P4-V2-R1I Integrated Harness Runtime 验收报告

**批次**：`P4-V2-RC-R1I-20260811`  
**计划**：`P4-V2-R1I.1–R1I.6 / phase4-v2-residual-r1`  
**结论**：顶层 durable runtime core accepted；真实 workflow/Sandbox/Approval adapters pending，`R4V2-RC-01` 保持 partial

## 实现证据

- `harness/types`：strict checkpoint、stage/state/outcome、pinned revisions、stage record 和 durable outbox contract；未知字段和 digest tamper fail closed；
- `harness/store`：InMemory/SQLite CAS，checkpoint 与 event 在同一事务提交，WAL/FULL/private permission、restart、recoverable listing；
- `harness/runtime`：Intake→Cognition→Approval→Execution→Memory→Assurance→Remediation→Reexecution→Reverification→Judge→Operations；
- side-effect stage 在调用前持久化 outbox；进程死亡后只允许 receipt reconciliation，未知 effect 进入 ManualReview；
- completion gate 强制 plan/memory/approval/artifact/acceptance/Judge/Operations pin、所有必经 stage、无 unresolved finding 和无 pending/unknown effect；
- `project_operations()` 从 durable checkpoint 构造 display-safe snapshot，不使用 demo snapshot。

## 分层验证

| 层次 | 证据 | 结果 |
|---|---|---|
| 功能 | strict encode/decode、unknown/tamper、CAS/event digest | PASS |
| 模块 | stage transition、revision pin、completion gate、bounded remediation | PASS |
| 集成 | 完整 Harness port chain、审批暂停/恢复、Operations round-trip | PASS |
| 综合 | 首次 Assurance finding→remediation→reexecution→reverification→Completed | PASS |
| 恢复/指标 | process death、无重复 execution、reconcile=1、unknown effect fail closed | PASS |

实际命令结果：`phase4-harness` 3/3、`phase4-offline` 45/45、`phase3-offline` 14/14。

## 未关闭边界

当前 system test 使用 typed deterministic ports 验证顶层语义，但尚未把每个 port 绑定到真实 Cognition/Memory/ProfessionalAssurance/Remediation/Judge workflow、Sandbox executor、ApprovalStore 和生产 UI assembler。R2X/R3A 将关闭这些适配器及真实产物证据边界；因此本报告不把 `R4V2-RC-01` 标为 verified/complete。
