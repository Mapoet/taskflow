# Phase 4 Production Composition Closure

**批次**：P4-PC0–P4-PC7  
**批准日期**：2026-08-12  
**当前状态**：PC0/PC1、PC2 跨 Store saga、PC3 typed adapter/LLM observability contract 与 PC5 conflict persistence 已实施；领域输入装配及其余生产闭环保持开放

## 已实施事实

- `Phase4ProductionComposition` 将 Intake、Cognition、PlanApproval、Execution、MemoryUpdate、Assurance、Remediation、Reexecution、Reverification、Judge、Operations 定义为 11 个不可缺失阶段。
- 普通 callback port 默认不是 production-ready；生产 adapter 必须提供 capability manifest。
- 有外部副作用的 production adapter 必须提供 reconcile，否则构造失败。
- ApprovalStore、ArtifactExecutor、FilesystemArtifactOracle、Sandbox receipt-backed port 提供明确 production capability manifest。
- Durable effect 支持合法 CAS：`Prepared→Unknown/Committed`、`Unknown→Reconciled/Committed`，terminal effect 必须绑定 receipt digest。
- SQLite MemoryStore schema v2 增加独立、摘要校验、可重启的 `memory_conflicts` store。
- Web HITL 移除默认 reviewer 和固定 token；未配置服务端 reviewer/session 时 fail closed，浏览器只从 session storage 提交 opaque session token。
- `SQLiteRunHarnessSaga` 持久化 Run/Harness 双侧 revision 与 digest pin，以 CAS 推进 `Prepared→Committed→Reconciled`；重启可恢复，任一侧漂移均 fail closed 到 `ManualReview`。
- production composition 只接受 `WorkflowHarnessStagePort`，以 `WorkflowAdapterKind + HarnessStage + implementation revision + configuration digest` 形成 capability manifest；历史 callback/manifest wrapper 无法进入生产组合。
- Cognition、Memory、Assurance、Remediation、Reverification、Judge 被声明为 LLM-driven stage，必须返回完整 `LLMInvocationManifest`；缺失或 telemetry 导出失败均进入 `ManualReview`。
- `TelemetryLLMInvocationObserver` 统一输出 GenAI span 与 duration/token/cache/cost metrics，并关联 provider/model/profile/prompt/route/calibration/fallback、harness stage、trace/span 和 memory pins。

## 未关闭边界

- 类型化 adapter ABI 和不可伪装 callback 门禁已完成；Cognition、Memory、Professional Assurance、Remediation、Judge 等具体 workflow 的领域输入 provider/Store assembler 仍需逐一绑定，不能把测试实现作为 Live 证据。
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
