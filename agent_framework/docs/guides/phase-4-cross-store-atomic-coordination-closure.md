# Phase 4 Cross-Store Atomic Coordination Closure

**批次**：P4-CSAC0–P4-CSAC9  
**批准日期**：2026-08-12  
**状态**：local durable-saga control plane verified；production command/HA partial

## 目标与一致性口径

独立 Run、Harness、Approval、Memory、Assurance Store 不伪装成单数据库 ACID。本批采用 durable saga、revision/digest pin、幂等键、重读确认、摘要链和不可逆状态 ManualReview，关闭“连续调用多个 Store 即视为原子提交”的错误口径。

## 工作包

| 批次 | 实现/验收 |
|---|---|
| CSAC0 | authority、commit/visibility/terminal point 与补偿边界 |
| CSAC1 | WAL/FULL journal、CAS transition、immutable history、receipt digest chain |
| CSAC2 | 类型化 participant、inspect/prepare/commit/confirm/compensate、capability manifest |
| CSAC3 | Approval request/decision revision 与 digest pin |
| CSAC4 | artifact/acceptance expected refs 与旧报告拒绝口径 |
| CSAC5 | Memory record/view/snapshot revision refs |
| CSAC6 | production builder 强制 coordinator 与五类 mandatory participant |
| CSAC7 | restart/idempotency/partial commit/manual-review 恢复 |
| CSAC8 | 七层测试 |
| CSAC9 | Phase 3/4 regression、trace/ledger/status 校准 |

## 已实现不变量

- Journal 使用 WAL/FULL，operation id 与 idempotency key 唯一，状态迁移使用 CAS。
- 每次 transition 写 immutable history，并以 previous digest 构造 receipt digest chain。
- Production readiness 要求 Run、Harness、Approval、Memory、Assurance 五类 participant 具备非空 capability manifest。
- Participant 必须从真实 Store 重读 revision/digest；callback/self-claim 不可注册为生产 participant。
- 重复 operation 只复用相同 idempotency binding；已 confirmed 操作不会再次 commit。
- commit/confirm 失败或 pin drift 进入 ManualReview；不可逆事实不自动回滚。

## 仍开放边界

- 当前 participant 是状态 pin/确认 participant，不替代各 workflow Store 自身的事务写入。
- Memory governance action、Approval consumption、Assurance terminal commit 尚未全部改造成 coordinator 主导的 command。
- ArtifactJournal、ProductionInputRepository、Judge report 仍需专用 participant。
- 跨进程 lease/fencing、批量 reconcile backpressure、PostgreSQL backend 和多节点 chaos 未认证。
- 因此本批不能宣告分布式 ACID 或 Phase 4 production Live 完成。

## 测试证据

- `phase4_cross_store_coordination`：journal、restart、幂等、partial commit→ManualReview、mandatory coverage。
- `phase4_production_dependencies`、`phase4_production_builder`：缺 coordinator/能力不完整 fail closed。
- 2026-08-12：targeted 3/3、`phase4-offline` **72/72**、`phase3-offline` **14/14**、`git diff --check` PASS。
