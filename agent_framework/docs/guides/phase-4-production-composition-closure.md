# Phase 4 Production Composition Closure

**批次**：P4-PC0–P4-PC7  
**批准日期**：2026-08-12  
**当前状态**：PC0/PC1、PC2 跨 Store saga 与 PC5 conflict persistence 已实施；其余生产适配器闭环保持开放

## 已实施事实

- `Phase4ProductionComposition` 将 Intake、Cognition、PlanApproval、Execution、MemoryUpdate、Assurance、Remediation、Reexecution、Reverification、Judge、Operations 定义为 11 个不可缺失阶段。
- 普通 callback port 默认不是 production-ready；生产 adapter 必须提供 capability manifest。
- 有外部副作用的 production adapter 必须提供 reconcile，否则构造失败。
- ApprovalStore、ArtifactExecutor、FilesystemArtifactOracle、Sandbox receipt-backed port 提供明确 production capability manifest。
- Durable effect 支持合法 CAS：`Prepared→Unknown/Committed`、`Unknown→Reconciled/Committed`，terminal effect 必须绑定 receipt digest。
- SQLite MemoryStore schema v2 增加独立、摘要校验、可重启的 `memory_conflicts` store。
- Web HITL 移除默认 reviewer 和固定 token；未配置服务端 reviewer/session 时 fail closed，浏览器只从 session storage 提交 opaque session token。
- `SQLiteRunHarnessSaga` 持久化 Run/Harness 双侧 revision 与 digest pin，以 CAS 推进 `Prepared→Committed→Reconciled`；重启可恢复，任一侧漂移均 fail closed 到 `ManualReview`。

## 未关闭边界

- Cognition、Memory Workflow、Professional Assurance、Remediation、Judge、Live 的 workflow-to-harness production adapters 尚需逐一实现，不能用 manifest test adapter 冒充真实生产 adapter。
- Run/Harness saga journal 已具备，但尚未由 Harness runtime 在每个 stage checkpoint 自动写入，故仍不能声称统一事实源。
- 真实 IdP/JWT/OIDC verifier、session rotation、KMS signer、四端交互仍未关闭。
- production hybrid memory retrieval、真实 domain/security/metric oracle、OTLP mTLS、nightly expert eval 仍开放。
- 用户明确暂缓的真实多节点 PostgreSQL/ObjectStore chaos 不属于本批阻断项。

## 下一实现顺序

1. workflow-to-harness typed adapters，并在每个 stage 自动写入 saga binding；
2. production identity provider；
3. hybrid memory retrieval/index generation pin；
4. real oracle/remediation/reverification vertical；
5. telemetry/eval scheduled production adapters；
6. 五端联合 crash/recovery 验收。
