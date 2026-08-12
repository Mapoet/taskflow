# Phase 4 Production Composition Closure

**批次**：P4-PC0–P4-PC7  
**批准日期**：2026-08-12  
**当前状态**：PC0–PC5、PC6A dependency graph 与 PC6I default production builder 已实施；PC7A 离线系统验收已纳入统一门禁，PC7C Live 外部认证仍须由真实基础设施证据关闭

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
- `CognitionWorkflowAdapter`、`MemoryWorkflowAdapter`、`AssuranceWorkflowAdapter`（含独立 Reverification 模式）、`RemediationWorkflowAdapter` 和 `JudgeWorkflowAdapter` 已直接调用对应真实 workflow class；`ProductionWorkflowInputAssembler` 提供强类型领域输入边界，adapter 校验 Harness pin，并从 durable `LLMRuntimeStore` 反查成功 invocation manifest，不能信任 workflow 自报的摘要。
- `StoreBackedProductionWorkflowInputAssembler` 组合 `PlanStore` 与 `ProductionWorkflowInputRepository`，按 tenant/task/run identity 和 Harness pin 装配 Intake、Memory source、AcceptanceContract、task context、artifact manifest、AcceptanceReport、AssuranceCheckpoint、ImpactInventory 与 Eval suite/runs/datasets；任一对象缺失或 identity 不一致即 fail closed。
- `SQLiteProductionWorkflowInputRepository` 以 kind + tenant/task/run + lookup digest + revision 为不可变主键，WAL/FULL 持久化完整领域输入；同 revision 异摘要拒绝、artifact/report 强制内容摘要寻址、读取时重算摘要、Eval DatasetRegistry 可跨重启重建，且所有 SQLite API 统一经 `internal/sqlite_utils.hpp`。
- `ProductionCompositionDependencies` 将 Run/Harness saga、统一输入库、Plan/LLM/Approval/Assurance/Memory/Remediation/Judge stores、RoleRuntime、Telemetry、Sandbox、Artifact executor、Operations assembler 及五类 LLM workflow 纳入显式依赖图；缺失、Sandbox 不可用、LLM store 身份漂移均 fail closed。
- `DefaultProductionCompositionBuilder` 是完整 11-stage composition 的唯一默认构建入口：内建 Cognition/Memory/Assurance/Remediation/Reverification/Judge 真实 workflow adapter，部署侧仅注入 Intake/Approval/Execution/Operations 四类边界 adapter；kind/stage/revision/configuration 任一不匹配均拒绝启动。构建结果同时固定 dependency、composition 与 deployment 三层 manifest digest。

## 未关闭边界

- 默认 composition wiring 已具备，但实际部署仍须实例化全部真实依赖并通过 Live provider calibration；离线 fixture 不能作为 Live 证据。
- Run/Harness saga journal 已具备，但尚未由 Harness runtime 在每个 stage checkpoint 自动写入，故仍不能声称统一事实源。
- 真实 IdP/JWT/OIDC verifier、session rotation、KMS signer、四端交互仍未关闭。
- production hybrid memory retrieval、真实 domain/security/metric oracle、OTLP mTLS、nightly expert eval 仍开放。
- 用户明确暂缓的真实多节点 PostgreSQL/ObjectStore chaos 不属于本批阻断项。

## 下一实现顺序

1. PC7C：真实 IdP/KMS/LLM/Sandbox/OTLP 部署注入和签名认证；
2. 在每个 stage checkpoint 自动写入 Run/Harness saga binding；
3. hybrid memory retrieval/index generation pin；
4. real oracle/remediation/reverification vertical 与 scheduled eval；
5. 五端联合 crash/recovery 验收。
