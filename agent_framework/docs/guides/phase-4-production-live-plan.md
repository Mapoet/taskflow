# Phase 4 R6L Production Live Certification 实施计划

**计划版本**：`phase4-r6l-production-live-r1`  
**状态**：approved / executing  
**批准日期**：2026-08-11  
**上游**：[Phase 4 v2](./phase-4-plan-v2.md)、[Residual Closure](./phase-4-residual-closure-plan.md)

## 1. 唯一目标

用真实 provider 和真实外部依赖执行不可静默跳过的角色矩阵，并由独立事实源重建执行证明，最终产生经 accountable approval、可信签名且未过期的 `executed=true` 报告。R6L 不以 mock、scripted RoleRuntime、普通绿色 CTest 或执行器自报字段替代生产证据。

## 2. 证据等级

| 等级 | 定义 | 可关闭 R6L |
|---|---|---:|
| `offline-control` | scripted/mock provider；验证 schema、状态机、CAS、负例 | 否 |
| `production-like` | 真实 provider/进程/依赖，在隔离测试环境运行 | 否；可作为候选报告 |
| `production-certified` | 获批环境与矩阵；全部 mandatory cell 实际执行；独立 evidence verifier；ApprovalStore 决策；生产 signer；报告未过期 | 是 |

`skipped`、`unavailable`、`blocked`、`inconclusive` 必须保留原义，均不得折算为 passed。

## 3. 不可变绑定

EnvironmentManifest 必须绑定 tenant、certification/revision、Git/build/config、OS/region/endpoint class、provider registry、memory/sandbox/telemetry/evaluation policy、外部依赖摘要、opaque secret references 和有效期。每个角色必须绑定 profile/prompt/provider/model/adapter/calibration revision、capability、region 和 independence group。

报告必须闭包引用 Environment、Matrix、Memory View/Snapshot、Plan、Approval、Invocation、Artifact、Oracle、AcceptanceReport、Judge/Eval 和 dependency digests。任一 mandatory revision 漂移、证据缺失、依赖替换、撤销或过期都使认证失效。

## 4. Mandatory matrix

- Cognition：investigator、strategist、planner、独立 critic；
- Memory：scope selector、retriever/reranker、consolidator、governance；
- Execution：受审批和 Sandbox 约束的 executor；
- Assurance：code、architecture、domain、security、completeness、resolver；
- Remediation：impact analyst、remediation planner、reverification planner；
- Evaluation：blind primary Judge、secondary Judge、adjudicator；
- Dependencies：LLM、MCP、A2A、IdP、Sandbox、renderer、telemetry。

planner/executor/verifier/Judge 必须满足已声明的身份与 independence-group 约束；verifier 只读、Judge blind、强 oracle 不得被 LLM 覆盖。mandatory cell 禁止静默 fallback。

## 5. 执行与信任边界

生产 runner 只负责按获批配置调度。`LiveCellExecutor` 的返回值是候选声明，不能单独建立 `executed=true`。生产 `CellEvidenceVerifier` 必须从不可变 Invocation/Audit/Artifact/Oracle 事实源独立核对：调用存在、spec/environment 绑定、时间窗、trace 连续性、output/artifact digest、required oracle、预算和结果。

credential 仅以 opaque reference 出现。报告、checkpoint、日志和 UI 禁止保存 secret value、raw authorization header、私有推理过程或未脱敏 memory/tool payload。

## 6. 签名、审批与发布

HMAC 仅保留为 offline/兼容 fixture。`production-certified` 使用非对称签名或 KMS/HSM signer，Verifier 只需公钥或 KMS verify 权限。key ID、算法和签名摘要进入报告；signer 与 approver 分离。ApprovalStore decision 必须绑定 environment、matrix、report signing digest、reviewer identity、scope、expiry 和 revocation 状态。

发布 gate 必须重新验证 contract canonical digest、全部 mandatory cells、独立 attestation、签名、approval、expiry 和 expected revisions。缺任一输入返回非零；生产 job 禁止 skip-as-pass。

## 7. 实施批次

| 批次 | 交付 | 退出门槛 |
|---|---|---|
| `R6L-R0` | 本计划及章程/status/trace/decision/ledger 统一 | 口径单一、历史基线与当前值分离 |
| `R6L-R1` | versioned environment/matrix bundle loader 与 validator | unknown field、digest/revision/role/dependency 缺失 fail-closed |
| `R6L-R2` | production runner、真实 RoleRuntime/MCP/A2A/Sandbox adapters | 不依赖 fixture；无凭据明确 BLOCKED |
| `R6L-R3` | durable attestation store 与独立 evidence verifier | 伪造/replay/cross-environment/missing oracle 全拒绝 |
| `R6L-R4` | asymmetric/KMS signer、ApprovalStore binding、required gate | signer/approver 分离；撤销/过期/漂移拒绝 |
| `R6L-R5` | scheduled runner、retention、alert、cost/concurrency guard | required job 无静默 skip，失败阻断发布 |
| `R6L-R6` | 真实矩阵执行与 AcceptanceReport | blocker-free、未过期、签名有效的 `executed=true` 报告 |

## 8. 五层验收

功能层验证 contract/digest/signature/expiry；模块层验证 cell/provider failure/budget；集成层验证真实 LLM/MCP/A2A/Sandbox/Telemetry；综合层执行完整 Harness；指标层记录 mandatory execution rate、evidence closure、false acceptance、recovery、latency、tokens 和 cost。生产报告要求 mandatory execution 与 evidence closure 均为 100%，跨 scope 泄漏和未经授权 memory promotion 均为 0。

## 9. 当前阻塞与解除条件

软件侧 R0–R5 可以持续实施。R6 的实际签发需要用户批准的 provider/profile/prompt/calibration matrix、opaque credential references、真实依赖 endpoints、accountable approver 和非对称/KMS signer。缺少这些事实时结果必须保持 `BLOCKED/inconclusive`，不得生成伪生产报告。
