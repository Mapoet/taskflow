# WP4.2：Durable Run Kernel

**优先级**：P0  
**依赖**：共享 schema  
**下游**：WP4.0、WP4.1、WP4.3、WP4.9、WP4.8

## 1. 目标

把分散的 SessionStore、Effect Journal、Memory generation、Skill/Child checkpoint 收敛为完整运行持久化语义。恢复单位是 Run/Plan/Node attempt，不只是会话消息。

## 2. 目标模块

```text
include/agent/run/{types,store,scheduler,replay,timer}.hpp
src/run/{types,sqlite_store,scheduler,replay,timer}.cpp
tests/phase4/durable/
```

核心对象：`RunState`、`NodeState`、`RunCheckpoint`、`PendingWork`、`DurableTimer`、`InterruptionRef`、`ReplayRecord`、`GraphDefinitionRef`、`MemorySnapshotRef`、`MemoryViewRef`。

## 3. 可执行任务

| ID | 任务 | 完成条件 |
|---|---|---|
| 4.2.1 | Run schema/state machine | Created/Running/Interrupted/Waiting/Completed/Failed/Cancelled，非法迁移测试 |
| 4.2.2 | RunStore interface | create/load/CAS checkpoint/list pending/append event/compact |
| 4.2.3 | SQLiteRunStore v1 | WAL、transaction、busy、private permission、migration、corruption handling |
| 4.2.4 | Graph definition registry | template/revision/digest；恢复时拒绝不兼容 graph |
| 4.2.5 | Node boundary checkpoint | cursor、inputs/outputs digest、attempt、pending successors、effect/memory snapshot/view refs |
| 4.2.6 | Durable retry/timer | backoff、deadline、clock abstraction、重启后补发且仅一次 claim |
| 4.2.7 | Interruption persistence | payload、policy、resume token、expiry、state digest |
| 4.2.8 | Deterministic replay | 记录非确定输入和已提交输出；副作用不在 replay 中裸重放 |
| 4.2.9 | Existing store bridge | Session/Effect/Child/Memory/Skill/Plan/Evidence refs 与 checkpoint 原子关联 |
| 4.2.10 | Recovery coordinator | 启动扫描、stale running、manual review、resume scheduling |
| 4.2.11 | Compaction/retention | checkpoint/event retention、artifact refs、GC 与 legal hold |
| 4.2.12 | Fault-injection E2E | 每个 commit 窗口 kill -9/restart；无丢失、无重复 effect |

## 4. 一致性边界

Run checkpoint 先持久化 node/effect/MemorySnapshot/View references，再推进 cursor；外部副作用继续使用 ToolEffectJournal 的 idempotency/lookup/manual-review，不承诺无法控制的系统 exactly-once。CAS conflict 不得覆盖较新 plan/acceptance/memory revision；恢复默认重建同一 View，若 provider revision 不可用则 fail-closed 或进入 migration/manual review。

## 5. 五层验收

- 功能：pause/restart/resume、retry/timer、cancel。
- 模块：状态机、CAS、migration、clock、serialization property tests。
- 集成：GraphExecutor、SessionStore、Effect WAL、ChildTask、HITL。
- 综合：规划或验收中途崩溃后恢复同一 task/plan/evidence/memory snapshot/view。
- 指标：恢复时间、checkpoint latency/size、timer drift、重复执行率=0（受控 fixture）。

## 6. DoD 与回滚

任一认知、记忆视图转换、执行、审批、验证节点被杀死后可从 committed boundary 恢复，且不丢证据、不静默切换 Memory revision 或重复不可幂等操作。先 dual-write SessionStore/RunStore，shadow recovery 对比通过后切换；保留旧 session-only 路径作为显式 legacy 模式。
