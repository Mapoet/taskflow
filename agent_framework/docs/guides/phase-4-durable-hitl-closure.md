# Phase 4 WP4.2/WP4.3 Durable Run 与 HITL Closure

**批次**：`P4-DRA0`–`P4-DRA8`  
**批准日期**：2026-08-12  
**实施状态**：本地生产控制面闭环已实现；外部 IdP/KMS/多节点事务不在本批证明范围

## 1. 决策与边界

- SQLite 内部以 `RunStore::commit` 提供 checkpoint CAS、graph cursor、event、effect、memory pin 和 interruption 的单事务提交。
- 外部工具、模型、对象存储不宣称跨系统 exactly-once；恢复时 `Prepared/Unknown` effect 必须先 reconcile，禁止 replay 直接重放副作用。
- time-travel 是只读历史投影；从历史点重新执行必须创建新 run/revision。
- UI 只提交 action intent。principal、roles 与 identity attestation 由服务端会话提供，客户端不能自报 reviewer 身份。
- quorum 是持久化多主体决策约束；没有 KMS/签名者证据时不宣称密码学多签。

## 2. 实施追溯

| 批次 | 实现 | 关键证据 |
|---|---|---|
| P4-DRA0 | 冻结上述事务、恢复、安全边界 | 本文档、`phase-4-status.md` |
| P4-DRA1 | 原子 `RunCommit`；effect fencing/idempotency；memory snapshot/view pin | `run/store.hpp`、`run/sqlite_store.cpp` |
| P4-DRA2 | event previous/event/state digest chain；历史 reconstruct/verify | `test_phase4_durable_run` |
| P4-DRA3 | `DurableRunCoordinator` store-first executor seam；不确定 effect 强制 reconcile | `run/durable_executor.*` |
| P4-DRA4 | durable delegation grant、scope/role/risk/time/revocation 校验 | `approval/executor.*` |
| P4-DRA5 | request-defined quorum、distinct principal、required role coverage、identity attestation journal | `test_phase4_approval_executor` |
| P4-DRA6 | revision supersession、防旧票重放、durable escalation | `test_phase4_approval_executor` |
| P4-DRA7 | authenticated action service；Web server identity 与 CSRF；客户端移除 reviewer 输入 | Web demo 与真实截图 |
| P4-DRA8 | restart、stale CAS、uncertain effect、delegation、quorum、edit、escalation、HITL resume 联合回归 | Phase 4 offline 门禁 |

## 3. 尚未扩大解释的事项

- `AccountableApprovalExecutor` journal 与 `ApprovalStore` 可部署在不同 SQLite 文件，因此 vote→final decision 使用 durable reconcile，而不是虚构跨数据库原子事务。
- Web demo 的服务端 principal 来自部署环境/会话配置；生产部署仍须由真实 IdP middleware 生成不可伪造 attestation。
- graph executor 已有强制恢复接缝，但旧执行路径不会自动获得 durability；生产 composition 必须显式使用 `DurableRunCoordinator`。
- 外部多节点 PostgreSQL、复制 ObjectStore、网络分区和主库切换仍按用户要求暂缓。

## 4. 验收命令

```bash
cmake --build build -j2
ctest --test-dir build -L phase4-offline --output-on-failure
ctest --test-dir build -R 'phase4_durable_run|phase4_approval_executor|web_ui_static_contract' --output-on-failure
git diff --check
```

UI 证据：`/tmp/taskflow-p4-dra-hitl.png`，来自真实运行的 `web_ui_demo --demo-state`，显示服务端身份说明和 canonical HITL 操作区。
