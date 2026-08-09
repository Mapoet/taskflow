# P4-F2D Durable Stores 增量验收报告

**Run**：`P4-F2D-DURABLE-STORES-20260809`  
**Plan revision**：`plan-v3.1`  
**决策**：`partial`——Evidence/Plan/Approval durable store 子集 accepted；P4-F2D 整体继续开放。

## 1. 已实现和验收

- `SQLitePlanningStore` 同时实现 EvidenceStore/PlanStore，在同一 SQLite WAL/FULL 数据库中保存 append-only evidence 与 immutable plan revisions；
- evidence 按 tenant/task scope 隔离，ID 和 locator+digest 去重，外部/RAG/tool evidence 禁止携带指令权；
- plan 支持跨实例恢复、CAS、连续 revision、父 canonical digest 校验和 stale writer 拒绝；
- `SQLiteApprovalStore` 持久保存 request/decision history，提供 tenant pending list；
- decision 强制绑定 request digest、tenant/task、policy/plan/arguments/artifact/memory-view，拒绝 self approval、过期 approval、stale revision 和非法终态迁移；approved 仅允许受控 revocation；
- 三个 SQLite store 都采用私有文件权限，并拒绝未来 schema version。

## 2. 实测证据

```bash
ctest --test-dir /tmp/taskflow-phase4-f0r-make.2XyicC \
  -L phase4-offline --output-on-failure
ctest --test-dir /tmp/taskflow-phase4-f0r-make.2XyicC \
  -L phase3-offline --output-on-failure
```

- Phase 4：13/13 PASS，0 failed；新增 `phase4_planning_store`、`phase4_approval_store`；
- Phase 3：14/14 PASS，0 failed；
- 合计：27 项确定性回归 PASS。

## 3. 残余风险

- AcceptanceReport/EvidenceLedger 尚未持久化；
- Run、Plan、Memory、Effect 位于独立数据库/日志，尚无事务协调或显式 recoverable commit protocol；
- Approval edit 尚未生成受控的新 request/revision；delegation、双人多签、escalation 和 UI 未完成；
- SQLite 适用于单机 durable kernel，不代表远程 HA store。

因此本报告不能把 WP4.0、WP4.2、WP4.3 或 Phase 4 标为 `[x]`。
