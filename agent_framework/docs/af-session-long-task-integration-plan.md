# AF Session / Long-task Integration Closure Plan

**计划 ID**：AF-SLT0–AF-SLT8  
**状态**：approved / in-progress  
**批准日期**：2026-08-15  
**依据**：`af-test.md` 数据库审计、Phase 4 状态、AF-CC-LTW 现有实现与真实 UI 运行证据

## 1. 目标与不可降低的完成口径

本计划不是继续增加孤立模块，而是把已经存在的 Conversation、Planning、Memory、
LongTaskWorkflow、Invocation、Approval、Assurance、Judge、TaskClosure 和 Operations
收敛成一条用户实际使用的、不可绕过的运行主链。

完成必须同时证明：

1. Session、Turn、Task、Run、Invocation、Closure 具有独立且可关联的 durable identity；
2. Model EndTurn、响应完成、Harness 结构完成、Invocation terminal 和 Task verified
   互不替代；
3. “继续”、补充要求、状态查询、暂停和取消作用于明确的 active task；
4. 长任务进入 durable LongTaskWorkflow，而不是一次进程内 ReAct callback；
5. 每个进度、等待、工具、产物、证据、验收和闭环状态可以 replay；
6. crash、late result、unknown effect 和 stale lease 不产生孤儿 running 或重复副作用；
7. 四端 UI 使用同一 durable projection，并清楚区分 candidate/unverified/verified；
8. `af-test.md` 的十项 Golden Tasks 和强制指标全部有直接证据。

## 2. 权威对象模型

```text
ConversationSession
  ├─ UserTurn
  ├─ ActiveTaskPointer
  └─ ContextProjection/CompactBoundary

PersistentTask
  ├─ RequirementRevision
  ├─ TaskRun / PlanRevision
  │    └─ LongTaskWorkflow
  │         └─ Invocation
  │              ├─ Progress/PartialResult
  │              ├─ Artifact/EffectReceipt
  │              └─ Lease/Fencing/Reconcile
  └─ TaskClosureDecision
```

### 2.1 终态权威

| 对象 | 允许的终止语义 | 不允许推导 |
|---|---|---|
| Model Turn | 输出停止、工具请求、等待、错误 | Task completed |
| Conversation Turn | response completed/failed/awaiting | Task verified |
| Harness | pipeline completed/blocked/manual review | 用户任务完成 |
| Invocation | completed candidate/failed/cancelled | Task verified |
| TaskClosure | completed verified/limited/blocked/failed/cancelled | — |

只有 `TaskClosureController` 可以签发 `completed_verified`。

## 3. 工作包

## AF-SLT0 — Contract and Truth Boundary

### 实施

- 增加 `TaskIdentity`、`TaskLifecycleState`、`TaskClosureState`、
  `TurnTaskLink` 和 `TaskRunLink`；
- profile-aware response completion policy；
- Execution success 要求 candidate、artifact 或 continuation 至少一个成立；
- lightweight Harness 终态命名为 pipeline completion，不得作为 task terminal；
- 所有生产/专业 profile 缺 production composition 时 fail closed；
- 为旧数据库提供版本化 decode/migration。

### DoD

- 空 candidate + 无 artifact + 无 continuation 必须失败；
- EndTurn 永远不能设置 verified；
- Harness completed 不能把 Operations 设置为 Passed；
- contract/unit/negative/restart 全部通过。

## AF-SLT1 — Durable ActiveTask Registry

### 数据模型

- `conversation_tasks`
- `conversation_active_tasks`
- `task_requirement_revisions`
- `task_turn_links`
- `task_run_links`
- append-only task event digest chain。

### 行为

- Initial request 可创建 Task；
- “继续”恢复 active task；
- 补充要求创建 requirement revision；
- status query 只读，不启动执行；
- cancel/suspend/replan 产生显式控制事件；
- 多 active candidate 时必须 clarification，禁止猜测。

### DoD

- restart 后 active task 可恢复；
- CAS 冲突不静默覆盖；
- unrelated new task 可显式切换；
- 普通继续不创建新的 task_id。

## AF-SLT2 — Real-time Unified Event and Coordination

- Harness commit observer 在每次 CAS 后实时发布，不做 run 后批量补发；
- Conversation/Task/Run/Harness/Invocation/Closure 统一 correlation envelope；
- Conversation 纳入 CrossStoreCoordinator；
- prepared/committed/reconciled durable journal；
- durable cursor、overflow、restart replay；
- UI 只消费 committed event。

### DoD

- stage commit 到 UI P95 小于 1 秒（本地）；
- crash injection 不产生 Conversation/Harness 终态分叉；
- replay 顺序、digest、revision 全部可验证。

## AF-SLT3 — Orphan Execution Recovery

- Execution outbox 绑定 invocation、owner、lease、fencing、heartbeat、provider operation；
- 启动及周期 sweeper；
- attach/query/reconcile/resume/failed/manual-review；
- Conversation failure/cancel 与 Harness/Invocation 协调；
- unknown non-idempotent effect 永不自动重放。

### DoD

- orphan running rate = 0；
- stale worker commit = 0；
- duplicate confirmed effect = 0；
- 九个历史 orphan 模式均有重现与恢复测试。

## AF-SLT4 — Conversation to LongTaskWorkflow

- LLM TaskClassifier + deterministic policy gate；
- simple response 与 long-running task 分流；
- Artifact/Code/External/Professional 默认进入 production composition；
- Cognition 产生 TaskContract/Plan/criteria；
- Plan node descriptor 驱动 LongTaskWorkflow/Invocation worker；
- TaskClosure 决定继续、修复、询问、阻塞或 verified。

### DoD

- 长任务主链不调用 `HarnessTurnAdapter`；
- 真实 PlanStore、InvocationStore、IncrementalResultStore、Closure receipt 可追溯；
- 缺 mandatory dependency 时启动失败，而非 fallback。

## AF-SLT5 — Steering, Background and Context Endurance

- task start/status/output/continue/cancel/attach/replan API/tool；
- queued input 与 requirement revision；
- clarification/approval resume；
- 自动 compact；
- bounded IncrementalResult/ObjectStore projection；
- compact 前后 mandatory state digest equivalence。

### DoD

- 两次 compact 后 contract/criteria/active invocation 不丢失；
- status 不创建执行 Turn；
- cancel 能传播并 reconcile；
- background task 可断线重连。

## AF-SLT6 — Unified Operations and UI

- 四端同源展示 Session/Task/Run/Turn/Invocation；
- Plan revision、criteria、active/waiting/blocked time；
- partial output、artifact、approval、finding、remediation、closure；
- durable cursor resume；
- 去除固定 demo run/task identity；
- response completed 与 task verified 使用不同徽标和文案。

### DoD

- CLI/TUI/ImGui/Web snapshot digest 对同一 revision 一致；
- Observation 在真实运行中实时更新；
- restart/reconnect 后不回退；
- 使用真实截图验证布局、逻辑和可操作性。

## AF-SLT7 — Golden, Recovery and Soak Certification

Mandatory scenarios：

1. 5–10 分钟多工具文件/图表任务；
2. 执行中“继续”和补充要求；
3. kill/restart；
4. MCP late result/reconcile；
5. Approval wait/resume；
6. 两次 compact；
7. stagnation/replan；
8. missing artifact；
9. empty assistant output；
10. Conversation/Harness coordination failure。

Mandatory metrics：

- orphan running = 0；
- empty-output completed = 0；
- state inconsistency = 0；
- duplicate side effect = 0；
- restart recovery = 100% mandatory matrix；
- false verified completion = 0；
- P95 first progress < 10 秒；
- heartbeat interval 5–15 秒。

## AF-SLT8 — Documentation and Completion Audit

- 更新 `af-test.md`、`agent-framework2claude-code.md`、
  `phase-4-status.md`；
- 需求—代码—测试—运行证据追溯矩阵；
- 明确 Offline/ProcessLive/ProviderLive 边界；
- 未取得真实 provider/IdP/KMS 证据时不得宣告 Production Live 完成。

## 4. 连续实施顺序

```text
SLT0 → SLT1
     → SLT2 → SLT3
     → SLT4 → SLT5
     → SLT6
     → SLT7 → SLT8
```

每批必须满足：

1. targeted tests；
2. Phase 4 offline regression；
3. schema/restart/negative evidence；
4. diff audit；
5. 涉及 UI 时真实运行截图；
6. 通过后自动进入下一批。

## 5. 当前基线与已知误差

- LTW0–LTW8 主要组件已实现，但 live demo 未使用；
- LTW9 仅部分关闭，ProviderLive/soak 仍不足；
- lightweight Harness 的 plan/assurance/judge 是 non-authoritative；
- Harness 事件当前在 run 结束后批量桥接，不是真实时；
- `conversation_id:active-task` 只是临时字符串，不是 registry；
- Operations live persistence 已加入，但 identity/replay 仍未统一；
- 历史 SQLite 含 failed/completed 分叉和 execution pending orphan；
- 当前 UI 截图只能证明布局可运行，不能证明长任务主链集成。

以上基线不得在后续状态文档中被较窄的单元测试覆盖声明替代。

## 6. 实施台账

### 2026-08-15 — SLT0/SLT1/SLT2 第一批

- `[x]` 新增 SQLite durable ActiveTask registry、requirement revision、Turn link、
  active pointer、CAS 和 append-only digest event chain；
- `[x]` 四个 live demo 的共同 bootstrap 已改为解析 durable active task，普通继续复用
  task/run，`/new` 显式挂起旧任务并创建新 task；
- `[x]` interactive Execution 对空 candidate 且无 receipt 的 EndTurn fail closed；
- `[x]` interactive Harness 使用 `pipeline_completed_unverified`，不再记录为 accepted；
- `[x]` Harness commit observer 逐次发布 committed event；修复 adapter 级全局 cursor
  导致第二个 Turn 丢失 Observation 的问题；
- `[x]` `conversation_task_registry`、`harness_turn_adapter`、Operations targeted tests；
- `[x]` CLI/TUI/ImGui/Web 四目标编译；
- `[x]` Phase 4 offline regression：88/88 passed。

尚未闭环、不得提前宣称完成：status/cancel 的只读/控制 API、TaskRunLink、跨 store
协调、历史 orphan sweeper、Production LongTaskWorkflow 主链接入、steering/compact、
四端任务中心、Golden/soak/真实 UI 截图和 ProviderLive 证据。

### 2026-08-15 — SLT1 closure / SLT2–SLT3 integration batch

- `[x]` `task_run_links` v2 schema、Run/plan/requirement revision 绑定与查询；
- `[x]` status 使用只读控制 Turn，不进入 LLM/Harness；cancel/suspend 使用 durable
  transition，continue/replan 可恢复 suspended Task；无 active Task 的控制请求 fail closed；
- `[x]` CrossStoreCoordinator 新增强制 `conversation_task_registry` participant，
  Task revision/digest 被 prepare/commit/confirm pin 保护；
- `[x]` InvocationOrphanSweeper：过期 lease 原子提升 fencing token；幂等且无外部
  operation 的 invocation 转为 takeover-ready；非幂等或可能存在外部 effect 的 invocation
  强制进入 Reconciling，禁止自动 replay；
- `[x]` stale completion 在 sweeper 接管后被 fencing 拒绝；
- `[x]` targeted integration 5/5、四 live UI 目标编译、Phase 4 offline 88/88。

SLT2/SLT3 的模块闭环已经具备，但 production composition 的启动/周期调度仍需在
SLT4 主链接入时将 coordinator 与 sweeper 设为 mandatory dependency；在该接线完成前，
不得把“类已实现”表述为“所有 live 任务自动恢复”。

### 2026-08-15 — SLT4–SLT6 production integration

- `[x]` 新增 `LLMTaskClassifier` 与 deterministic policy gate；Production 的低置信度、
  无效结构化输出和缺失 classifier 均 fail closed，Demo 才允许确定性回退；
- `[x]` `HarnessSupportedTurnRuntime` 将 Artifact/Code/External/Professional 独立路由到
  `LongTaskWorkflow`，Production 缺 long-task executor 时不回退到交互 Harness；
- `[x]` 默认 production builder 直接构造 `LongTaskExecutionWorkflowAdapter`，Execution /
  Reexecution 不再接受 deployment callback 冒充生产执行器；Plan digest pin、类型化 descriptor、
  production-origin executor registry 与 dispatcher 构成不可绕过边界；
- `[x]` production dependency gate 强制 TaskRegistry、orphan sweeper、LongTask dispatcher /
  timer worker；启动时执行 orphan sweep 和到期 durable timer，失败拒绝 ready；
- `[x]` Harness 新增 `AwaitingExternal` durable 非终态语义，长任务可以释放交互 Turn，
  由 invocation event/timer 唤醒后继续，而不滥用 Approval；
- `[x]` `TaskControlService` 将 TaskRunLink、InvocationStore、Progress 与 ExecutionControlStore
  组合为只读 status/partial-output 和跨 active invocation cancel；Production 未注入则启动失败；
- `[x]` 修复 Operations invocation cursor 的全局 revision 错误：cursor 现按 invocation ID
  隔离，第二及后续工具从 sequence=1 开始也能实时推进 Observation；
- `[x]` 四端 Operations snapshot 增加 conversation/task/run/turn 同源身份；Web 真实运行截图
  `/tmp/af-slt6-web-final.png` 已人工检查，无重叠，Plan 卡片可见 Task/Run/Turn；
- `[x]` targeted tests、五个 demo 编译、Phase 4 offline **90/90 PASS**；LTW recovery
  certification 固定种子 2000-cycle soak PASS。

### SLT7/SLT8 认证边界

- `[x]` Offline：分类/路由、durable workflow、Task controls、orphan fencing、recovery matrix、
  Operations replay、空输出与 false-verified negative gate 已覆盖；
- `[x]` ProcessLive：真实 Web binary + browser 截图完成 UI 身份/布局验证；
- `[ ]` ProviderLive：当前进程未配置 OpenAI/Anthropic/通用 LLM provider 凭据，无法执行
  5–10 分钟真实 provider/MCP late-result campaign。该 cell 保持 `NotCertified`，不得以
  Offline soak 或 demo fixture 替代；
- `[~]` 因 ProviderLive mandatory cell 尚缺 evidence digest，AF-SLT 总状态保持 partial，
  但本计划要求的代码接线、离线恢复和文档审计已闭合。

### 2026-08-15 — Operations retention/restart hotfix

- `[x]` 修复固定 run 多次启动导致 `invocations` 无界累积，而 reader 在 256 项硬上限
  抛出 `invalid object array: invocations` 的读写契约不对称；
- `[x]` bounded compaction 保留全部 active invocation 和最新 terminal history，并同步压缩
  per-invocation replay cursor；`invocations_compacted` 提供累计审计计数；
- `[x]` 旧 snapshot 先验证原 digest，再确定性压缩并追加新 snapshot，历史行保持不可变；
- `[x]` 四个 demo 增加顶层异常边界，损坏状态明确报错并返回 2，不再 abort/core dump；
- `[x]` 修复 Harness stage output digest 被引用但没有 Evidence object 的 restart-invalid projection；
- `[x]` 真实 `/tmp/taskflow-web-ui-phase4-1357/operations.sqlite3` 从 280 条恢复为 255 条，
  压缩计数 28；targeted 3/3、Phase 4 offline **90/90 PASS**。

### 2026-08-15 — Interaction Projection stream CAS hotfix

- `[x]` 删除 Web demo 跨 conversation 共用的单值 `interaction_revision`；提交器现在按
  `(tenant_id, conversation_id)`读取 durable head；
- `[x]` 新增通用 `commit_interaction_projection`：每次基于当前 revision/head 重建
  event、node、edge，CAS 冲突最多重试 4 次，Invalid/Corrupt/Storage 仍 fail closed；
- `[x]` Interaction snapshot/events/node HTTP API 跟随当前 conversation，不再固定查询
  `default` stream；
- `[x]` live projection 重试耗尽时保留模型 candidate answer，Operations 标记
  `interaction_projection_degraded`，不再把辅助投影异常改写为 SYSTEM 对话；
- `[x]` 测试覆盖 default→真实 conversation、独立 stream revision、竞争提交自动重试；
  targeted 3/3、Phase 4 offline **90/90 PASS**；
- `[x]` 真实 Web binary/API 验证通过，截图 `/tmp/af-interaction-cas-fixed.png`；页面布局、
  Conversation 与 Tool activity 正常，无 projection SYSTEM 错误。
