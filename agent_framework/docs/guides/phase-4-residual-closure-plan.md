# Phase 4 v2 Residual Closure：生产集成闭环计划

**计划版本**：`phase4-v2-residual-r1`  
**状态**：approved / executing；PC0–PC6I、COBS0–COBS9 已并入当前事实基线
**批准**：用户于 2026-08-11 明确批准 `R0→R7` 连续实施，测试通过后自动进入下一批  
**上游计划**：[Phase 4 v2 计划](./phase-4-plan-v2.md)  
**事实基线**：[Phase 4 状态](./phase-4-status.md)  
**追溯**：[需求—计划—执行—证据矩阵](./phase-4/traceability-matrix.md)

## 1. 立项事实

F1L–F8U 已建立 Role Runtime、认知规划、多层记忆、专业验收、修复规划、Judge、Live 认证控制面和统一 UI；后续 PC/COBS/SIC 已补齐 11-stage production composition、typed adapters、durable input repository、生产 ToolBus Investigator、闭环修复审批与扩展 telemetry/SLO。2026-08-12 的 SIC9 全量 `phase4-offline` 为 **70/70 PASS**，但仍只证明 offline-control，不替代 production certification。

历史 `phase4_vertical` 仍只是早期手工 fixture，不能作为当前 production composition 的端到端证明。SIC 已关闭修复闭环，PAO 已补 production Oracle 控制面，CSAC 已补 WAL/FULL coordination journal、五类 Store revision/digest participant 与生产 mandatory gate。当前最短板转为 coordinator 主导的 command participant（Approval/Memory/Assurance/Artifact/Input/Judge）、生产校准/benchmark 与外部 Live 认证。

SIC0–SIC8 的当前增量已完成以下本地控制面收口：每次 Harness checkpoint 自动通知 Run/Harness saga；Cognition 使用摘要/身份/策略/有效期绑定的 ApprovalStore resolver；新增 store-backed Intake、Approval、Artifact Execution、Operations typed boundary adapters；finding 路径调整为 `Remediation → PlanApproval → Reexecution → Reverification`，并在进入修复时清除旧 approval/report pin；artifact lineage 与有界失败路径有系统测试；telemetry spool 具备 backoff、容量、重试上限、dead-letter、retention，SLO 具备 quantile 与 multi-window burn-rate。SIC9 已完成本地全量门禁；production Live 仍须单独认证。

GPC/GPW 随后关闭了旧 AgentLoop 与生产 Harness 双轨中的“完成权威”缺口：production profile 必须提供 closure binding 和 manifest 摘要，五个 LiveRuntime 入口传播同一信任配置，A2A/ChildTask 不接受远端或子任务自报完成，Harness revision 自动进入 progress ledger。本地工程门禁更新为 `phase4-offline` 73/73；R6L 外部认证仍独立开放。

Residual Closure 不新增平行演示系统，而是把现有模块收敛到同一生产路径。

## 2. 目标架构

```text
Phase4HarnessRuntime
  Intake → Cognition → Plan Approval → Execution
    → Memory Update → Professional Assurance
      → [Accepted] Judge / Operations / Complete
      → [Finding] Remediation → Approval → Re-execution
          → Artifact Rebind → Selective Re-verification
  
Durable Control
  Harness checkpoint + transactional outbox + idempotency/fencing
  + pinned plan/memory/profile/prompt/approval/artifact/report revisions

Execution/Evidence
  SandboxProvider → ArtifactManifest → deterministic oracles
  → LLM professional interpretation → deterministic Arbiter
```

控制面不承诺跨外部系统 exactly-once。外部 effect 继续使用 idempotency key、effect lookup、fencing 和 reconciliation；不确定结果进入 ManualReview。

## 3. Closure requirements

| ID | 必须达到的结果 | 主要关闭对象 |
|---|---|---|
| `R4V2-RC-00` | 残余需求、证据边界、批次和总 DoD 有单一事实源 | 治理漂移 |
| `R4V2-RC-01` | 所有 Phase 4 workflow 由一个 durable Harness 状态机编排，不能从生产完成路径绕过 | R4V2-01/02/03/04/05/06/08 |
| `R4V2-RC-02` | finding 实际触发受控修复执行、新 artifact、影响失效和二次 Assurance | R4V2-04/05 |
| `R4V2-RC-03` | ApprovalStore/PDP/identity/SoD/expiry/resume、Memory 治理和 UI action 使用同一事实源 | R4V2-02/03/05/08 |
| `R4V2-RC-04` | 不受信执行统一进入具体 Sandbox；trace/metric 可导出并有 SLO gate | R4.4/R4.5 |
| `R4V2-RC-05` | 固定真实数据集、人工基线、校准、nightly、签名趋势报告阻止质量回归 | R4V2-06 |
| `R4V2-RC-06` | 至少一个批准的真实 provider/外部依赖组合产生未过期 `executed=true` 认证 | R4V2-07 |
| `R4V2-RC-07` | 远程 durable queue、worker lease/fencing、共享 Store 和故障恢复通过 HA/chaos | R4.8 |

## 4. 连续实施批次

### R0 — 基线与治理

交付：本计划、状态/追溯/决策/台账更新、11 条总 DoD 的证据口径。  
退出门槛：文档链接有效；任务 ID 唯一；不把 planned/fixture/live-blocked 写成 verified。

### R1I — Integrated Harness Runtime

候选落点：`include/agent/harness/`、`src/harness/`。

| ID | 任务 | 验收重点 |
|---|---|---|
| `P4-V2-R1I.1` | Harness contract/state machine | strict schema、合法迁移、terminal fail-closed |
| `P4-V2-R1I.2` | Durable Harness Store | SQLite CAS、checkpoint、event、outbox、idempotency |
| `P4-V2-R1I.3` | Workflow port/adapters | Cognition/Memory/Approval/Executor/Assurance/Remediation/Judge/UI 统一端口 |
| `P4-V2-R1I.4` | Revision pinning | plan/view/profile/prompt/approval/artifact/report digest 闭包 |
| `P4-V2-R1I.5` | Recovery/reconciliation | 任意 stage kill/restart、重复投递、stale writer、unknown effect |
| `P4-V2-R1I.6` | Mandatory completion gate | 未执行、无强 oracle、未解决 finding、未批准副作用不得 Completed |

退出门槛：一个任务经同一 runtime 到达 terminal；强 oracle 失败不能完成；每个 stage 可恢复且不重复已确认 effect；Operations snapshot 来自 Store，而非 demo snapshot。

### R2X — Execution / Remediation / Re-verification

| ID | 任务 | 验收重点 |
|---|---|---|
| `P4-V2-R2X.1` | ArtifactExecutor contract | action capability、sandbox spec、idempotency、rollback |
| `P4-V2-R2X.2` | Artifact manifest/digest | producer、dependency、workspace diff、content digest |
| `P4-V2-R2X.3` | Real deterministic oracles | repo/file/build/test/runtime/security/metric observation |
| `P4-V2-R2X.4` | Remediation execution | 只执行批准 action；新 artifact 回填；旧 evidence 失效 |
| `P4-V2-R2X.5` | Selective re-verification | forced oracle 必跑；可复用 evidence 校验 digest/freshness |
| `P4-V2-R2X.6` | Bounded loop/system test | 修复成功、修复失败、回滚、循环上限、ManualReview |

退出门槛：故意制造“测试通过但产物不完整”，首次 Assurance 拒绝；执行最小修复后生成新 digest，二次 Assurance 只在强证据闭合时接受。

### R3A — Approval / Memory / Operations Integration

| ID | 任务 | 验收重点 |
|---|---|---|
| `P4-V2-R3A.1` | Accountable approval executor | reviewer identity、PDP、SoD、expiry/revocation、CAS |
| `P4-V2-R3A.2` | Edit/delegation/multisig | edit 产生新 request/revision；高风险双人复核 |
| `P4-V2-R3A.3` | Durable resume | decision 与 pinned request/state digest 一致后恢复 |
| `P4-V2-R3A.4` | Memory governance binding | promotion/forget/correction 关联 evidence + approval + propagation |
| `P4-V2-R3A.5` | Store-backed Operations assembler | 多 Store revision join、redaction、snapshot/replay |
| `P4-V2-R3A.6` | UI action integration | Web/CLI/TUI/ImGui 显示同源；Web action 进入真实 ApprovalStore |

退出门槛：demo controller 不参与生产路径；stale/越权/自审批/过期 decision 全部拒绝；真实 UI 运行截图覆盖 pending、approved、rejected、expired 和 resume。

### R4O — Sandbox / OTLP / SLO

交付具体 Process SandboxProvider、credential reference broker、network/file/resource enforcement、workspace snapshot/diff；OTLP batch exporter、W3C trace propagation、Audit bridge、SLO registry 和 release gate。  
退出门槛：逃逸/凭据泄漏/网络越权为 0；collector loopback 证明跨进程 trace；latency/cost/error/recovery SLO 可阻断发布。

### R5E — Dataset / Calibration / Nightly

交付 unit/repository/domain/adversarial/recovery/live 六层版本化数据集、人工 label/inter-rater 基线、planner/memory/assurance/Judge 指标、nightly scheduler、flaky quarantine、签名报告和趋势比较。  
退出门槛：任一生产 profile/prompt/model revision 有固定基线、置信区间、批准与回滚；Judge 不能覆盖强 oracle。

### R6L — Production Live Certification

使用批准的 provider/profile/prompt/calibration、IdP/MCP/A2A/Sandbox/renderer 依赖和 KMS signer 执行完整 cognition→memory→execution→assurance→judge 矩阵。  
退出门槛：报告 `executed=true`、所有 mandatory cell executed/pass、签名有效、未过期且无 blocker。缺凭据或依赖必须保持 `BLOCKED/inconclusive`，禁止 skip-as-pass。

执行口径与 R0–R6 批次以 [R6L Production Live Certification 实施计划](./phase-4-production-live-plan.md)为单一事实源。`LiveCellExecutor` 自报的 invocation/result 只作为候选声明；生产 `executed=true` 必须由独立 Invocation/Audit/Artifact/Oracle 事实源重建。HMAC 只属于兼容测试，生产认证要求非对称或 KMS/HSM signer，并与 accountable approver 分离。

### R7D — Distributed / HA

交付远程 durable queue、worker registry/heartbeat/scheduler、lease/fencing/idempotency、PostgreSQL 或等价共享事务 Store、object artifact/evidence store、tenant quota、rolling migration 和 chaos tests。  
退出门槛：worker/server/leader 故障不丢 run、不重复已确认 effect；stale fencing token 永远不能提交；共享状态经多进程恢复。

## 5. 每批强制测试层次

每批同时给出：

1. 功能：contract、非法状态、digest、policy；
2. 模块：stage、budget、retry、checkpoint、manifest；
3. 集成：跨 workflow/store/sandbox/telemetry/UI 边界；
4. 综合：任务输入到最终验收或明确 ManualReview/Blocked；
5. 指标：误接受、泄漏、重复 effect、恢复率、成本、延迟和稳定性。

测试名中区分 `offline`、`loopback`、`system`、`live-production`。只有真实执行并验证 EnvironmentManifest 的测试才能进入 production evidence。

## 6. 全局关闭门禁

最终逐条验证 Phase 4 章程的 11 条完成定义。任何一条缺直接证据，Phase 4 保持 partial：

- 真实调查后规划；专业 DAG/AcceptanceContract；动态多层 Memory View；
- cognition/plan/execution/approval/memory/acceptance 跨进程恢复；
- 新证据→replan；五层验收识别总体不完备；无证据拒绝虚假完成；
- 统一 sandbox/HITL；标准 trace/metric/cost/SLO；固定评测与发布门禁；
- 至少一个 production-like Live 全链路认证。

R7D 是 Phase 4 分布式目标的 mandatory closure；外部 R6L 未满足时可继续实施 R7D 的本地/loopback 能力，但不能宣告 Phase 4 完成。
