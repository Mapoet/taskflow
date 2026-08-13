# Agent Framework 对照 Claude Code 的系统性升级报告

**文档性质**：架构审计、目标架构与实施蓝图

**基线日期**：2026-08-13

**Agent Framework 基线**：`/home/Mapoet/projects/taskflow/agent_framework` 当前 Phase 4、GPC、GPW 与 CTR 事实状态

**Claude Code 基线**：`/home/Mapoet/projects/claude-code-source-code/src`，`@anthropic-ai/claude-code` 2.1.88 发布包解包源码

## 0. 核心结论

Agent Framework 已经不是简单的 C++ ReAct 示例，而是由 LLM 认知平面、Taskflow/Workflow 执行平面、Durable Run/Harness 控制平面、Approval/Sandbox 安全平面、Memory/Artifact/Evidence 数据平面和 Operations/Telemetry 运维平面共同组成的生产运行时原型。它在确定性任务完成、专业验收、修复复验、跨 Store 摘要绑定和生产认证控制方面，已经超过 Claude Code 客户端源码所呈现的边界。

Claude Code 在另一组问题上更成熟：会话级运行时、单轮 Agent 状态机、流式事件协议、工具生命周期、权限交互、动态工具池、上下文投影、compact boundary、JSONL 消息链、子 Agent 权限重建、后台通知和交互产品一致性。这些机制不直接证明任务完成，却显著提高了 Agent 在真实长会话中的连续性、响应速度、可恢复性和用户可控性。

因此，正确升级方向不是把 Agent Framework 改写成 Claude Code，也不是用巨型 C++ `queryLoop` 替代 Workflow/Harness，而是：

> 保留 Agent Framework 的确定性完成权威和 durable workflow，以 Claude Code 的 Conversation、Turn、Tool、Context 工程补齐交互式执行内核。

目标系统必须明确分离两个循环：

1. **Turn Loop**：完成一次模型—工具—观察循环，追求低延迟、流式反馈和上下文连续性；
2. **Task Closure Loop**：跨多个 Turn、阶段和进程推进 AcceptanceContract，追求证据闭合、可恢复执行和可信终态。

Claude Code 的弱点是 Turn 结束容易成为事实上的任务结束；Agent Framework 已用 GPC 的 `TaskClosureController` 解决了这一问题。CTR0–CTR10 及后续 closure 已建立轻量、类型化、事件驱动的 `ConversationEngine`，并完成 SQLite 原子边界、durable inbox、统一 Event Subscription/Replay、shared-Store 跨进程 fan-out，以及内容寻址的 event retention/archive/compaction。当前中心任务已转为 Long-running Tool Workflow：让 LLM 能在数分钟至数小时的复杂工作中持续启动、观察、等待、重规划、恢复和验收工具执行，而不是仅拥有返回 `future<json>` 的异步调用接口。`ProductionTaskRuntime` 与 `TaskClosureController` 继续独占任务终态权威。

### 0.1 2026-08-13 技术缺口状态

状态口径：`[x]` 已闭环且有测试证据；`[~]` 核心契约已实现但生产主路径未闭环；`[△]` 已有分散能力但尚未统一；`[ ]` 尚未实现。

| 工作包 | 状态 | 当前实现证据 | 仍需关闭的生产缺口 |
|---|---:|---|---|
| AF-CC0 契约冻结 | `[~]` | Conversation/Turn、`RuntimeEventEnvelope`、ContextProjection、CompactBoundary、六档 `TaskExecutionProfile` 已类型化；`ModelTurnOutcome` 不可签发 verified；queued input pin profile 与 iteration/token budgets | 缺统一版本化 `ToolContract`；权限、工具 generation、Memory/Skill/Prompt/Profile 与 deployment revision 尚未完整 pin 到每次 invocation manifest |
| AF-CC1 ConversationEngine | `[~]` | start/continue/interrupt/resume、Turn CAS、SQLite WAL/FULL Store、append-only parent/digest 消息链、原子 Turn boundary、跨连接线性化 sequence、durable input inbox、queued input 原子 FIFO 消费/确定性新 Turn/profile-budget 恢复、Store-backed Event Subscription/Replay、shared-Store 跨进程 pull-through fan-out、持久化单调 head/floor、内容寻址 archive、可恢复 Saga、安全 compaction、真实 `cursor_expired`、SDK typed callback/独立 cursor、重启恢复与 100 Turn 测试已完成；四个交互 demo、AgentServer/A2A 已接入受控执行边界 | 运行中 provider/tool 取消传播、Conversation ContextProjection 驱动真实 invocation 与更完整进程 crash/busy/disk matrix 尚缺；大规模多节点仍需 PostgreSQL notification/消息总线增强 |
| AF-CC2 Tool Lifecycle | `[△]` | ToolBus、schema、Approval、Sandbox、Effect Journal、MCP、Skill policy 与 observer 已分别存在 | 缺不可绕过的版本化 Tool Contract、固定执行管线、改写后重验、统一 retention/CAS 与 reconciliation |
| AF-CC3 Context/Compact | `[~]` | Conversation projection/boundary schema、mandatory contract/policy/citation 保留校验和 boundary 持久化已完成；既有 Agent memory compaction 已有自动/manual trigger、隔离 compaction LLM、fallback/cancel/timeout 测试 | Memory compaction 不等于 Conversation compact；ContextProjection 尚未驱动真实模型 invocation，缺 Conversation 自动 compact、CAS 大结果外置、确定性 fallback 与恢复等价性 |
| AF-CC4 三链持久化 | `[~]` | Conversation、Run/Harness、Effect 均有独立 Store/digest；已有 Run-Harness saga、CrossStoreCoordinationJournal、Effect reconciliation 与 restart 测试 | Conversation 尚未纳入统一 correlation/coordination boundary；缺全 durable 写点 crash injection、deterministic replay/time-travel 和 orphan reconciliation |
| AF-CC5 子 Agent 隔离 | `[△]` | ChildTask/A2A 已禁止远端或子任务自报完成直接关闭父任务 | capability intersection、独立 transcript namespace、父取消传播、远端证据本地强复验仍未形成统一内核 |
| AF-CC6 Streaming/安全调度 | `[△]` | 已有同轮只读工具并行、A2A submit 并行和 Taskflow 执行基础 | 缺 effect/concurrency taxonomy、冲突 DAG、流式提前调度、abort 后 effect 协调和 critical-path 证据 |
| AF-CC7 Experience/Operations | `[~]` | 五个 LiveRuntime demo 共享 bootstrap/profile；CLI/Web/TUI/ImGui 经公共 Conversation adapter；AgentServer/A2A 接入 ConversationStore；SDK 已有 typed runtime-event callback、cursor 与断线 replay；Operations 明确 candidate/verified | 各端尚未完全统一为同一 Operations snapshot/action contract；Approval 动作、取消、后台通知与跨进程续传仍有入口差异 |
| AF-CC8 质量/Live | `[~]` | Phase 4 offline 77/77、Phase 3 22/22、A2A 4/4、五 demo 构建通过；已有 Role Live Certification、production bundle/signature/attestation/approval/runner 与 fail-closed negative/restart 测试 | 认证框架存在不等于生产环境已认证；真实 provider/MCP/IdP/KMS/Sandbox 证据与 mandatory live matrix 尚未关闭，不能以 fixture/offline pass 替代 |
| AF-CC-LTW 长时复杂工具工作流 | `[~]` | LTW0–LTW3 已完成；LTW4 已形成 production-gated typed adapters；LTW5 已形成 Invocation/Effect/Object/Run/CrossStore commit coordinator；LTW6 已闭环 durable wait-observe-replan；LTW7 已形成 tenant-scoped durable incremental stream、CAS chunk/manifest、写前脱敏、bounded LLM result view、Worker/Invocation 集成和 production dependency gate；Phase 4 offline 78/78 通过 | LTW4/5 残余仍在；LTW7 尚需 ObjectStore delete/list 能力后才能完成物理 GC/归档删除；LTW8–LTW9 尚缺，现状仍不能声称已具备生产级长时自主工作能力 |

### 0.2 当前已确认的 Conversation 实现风险

1. `[x]` Turn 初始/终止 checkpoint、message 与 durable event 已统一进入 `ConversationCommit`；半提交窗口已在当前 SQLite Store 边界关闭。
2. `[x]` message/event/input sequence 与 parent 校验已移入 `BEGIN IMMEDIATE`，双连接竞争测试证明同一 parent 只有一个 writer 成功。
3. `[x]` Append/QueueNext/InterruptAndReplace 已使用 durable input inbox 区分；Append 才进入当前消息链，QueueNext/Replace 重启后仍为 queued，Replace 同事务中断当前 Turn。
4. `[~]` queued input 已实现 FIFO 原子 claim/consume、确定性下一 Turn、重启恢复、双连接竞争隔离与有界 drain；取消传播仍未连接正在运行的 provider/tool executor。
5. `[x]` 已实现 durable sequence cursor、Store replay、本地低延迟 publish、shared-Store 跨进程 pull-through fan-out、慢订阅者隔离、持久化单调 head/floor、内容寻址归档链、失败后幂等恢复、安全 compaction、真实 `cursor_expired`、AgentServer SSE `Last-Event-ID` 续传和 SDK typed callback；大规模多节点通知仍可增强。
6. `[~]` ContextProjection/CompactBoundary 当前是契约和 Store 能力，尚未接入模型 invocation；GraphTurnAdapter 已去除完成权威并成为现有入口的受控兼容边界，但 `AgentLoopNode` 尚未原生采用 TurnStateMachine/ContextProjection。

### 0.3 AF-CC1R 行动进度

2026-08-12 已完成 AF-CC1R 第一批：

- 新增 `ConversationCommit`，将 Turn CAS、message append 和 durable event append 收敛到一个 Store 事务边界；
- SQLite 实现使用 `BEGIN IMMEDIATE`，在持有数据库写锁后读取 message tail/event tail，并在数据库事务内分配连续 sequence；
- ConversationEngine 的 `turn_started` 与 `model_stop` durable boundary 已改用原子 commit，不再采用 checkpoint/message/event 三次独立提交；
- 双 SQLite 连接竞争同一 parent 时只允许一个 writer 成功，失败 writer 不留下 Turn、message 或 event；
- parent mismatch 故障注入验证整个 batch 回滚；
- 第二批新增 durable `conversation_inputs` inbox；Append、QueueNext、InterruptAndReplace、ControlAction 不再共享同一 append 语义；interrupt checkpoint/event 也进入原子 commit；
- queued replacement/next-turn input 经 Store 重启仍可恢复，且不会污染当前模型消息链；
- 回归证据：`phase4_conversation_runtime` PASS，Phase 4 offline 74/74 PASS，Phase 3 22/22 PASS。

仍未关闭的 AF-CC1R 范围：未来 tool/effect reference 尚未纳入同类事务 API；需要继续增加真实进程 crash injection、busy/timeout、磁盘故障和 schema migration 测试。AF-CC1W 的入口迁移、跨进程事件交付及 retention/archive/compaction 已闭环，但原生模型上下文和取消传播仍未完成，因此 AF-CC1 总状态保持 `[~]`，不能提升为 `[x]`。

### 0.4 AF-CC1W 行动进度

2026-08-12 已开始入口迁移的共同适配层：

- `GraphTurnAdapter` 已支持把既有 `WorkflowResult` 映射为类型化 `ModelTurnOutcome`；
- success、provider failure、guard stop 均保持 `task_completion_verified=false`，错误文本只能作为 candidate/diagnostic；
- 该 adapter 是 `run_react_cli_sync` 与 ConversationEngine 之间的受控兼容边界，不是新的完成权威。

本批已完成：

- CLI/Web/TUI/ImGui 四个交互 demo 均通过公共 `run_conversation_turn` 进入 SQLite ConversationStore/ConversationEngine；`run_react_cli_sync` 只存在于受控 callback 后；
- 公共 composition 统一读取 `AGENT_CONVERSATION_DB`、`AGENT_TENANT_ID`、`AGENT_CONVERSATION_ID`，并生成单调不重复 Turn identity；
- Web 将 typed `RuntimeEventEnvelope` 投影为 `runtime_event`，其余端保持现有流式 UI sink；
- AgentServer 在真实 `execute_sync` boundary 接入可注入 ConversationStore，A2A task ID 映射为 Turn ID、session ID 映射为 conversation ID；SDK 已暴露 typed runtime event callback 与独立 cursor；
- `agent_server_demo` 默认启用独立 durable conversation DB；生产 task 是否 COMPLETED 仍由原有 closure/verified authority 决定，Conversation outcome 不得越权；
- 四个交互 demo 与 agent_server_demo 均构建通过；A2A 4/4、Phase 4 offline 74/74、Phase 3 22/22 PASS。

后续补充完成：durable queued input 已实现 FIFO 原子 claim/consume、确定性新 Turn、profile 与 iteration/token budgets 恢复、双 SQLite 实例竞争隔离、有界 drain 和重启幂等；input 状态、Turn、用户消息与 `user_input_claimed`/`user_input_consumed`/`turn_created_from_queued_input`/`turn_started` 在同一事务提交。

尚未完成：运行中 provider/tool 取消传播与完整真实进程 crash/busy/disk matrix。SDK typed `RuntimeEventEnvelope` callback、独立 runtime cursor、持久化 retention floor、内容寻址归档、安全 compaction、queued input 跨 Turn 原子消费和 shared SQLite Store 跨进程 fan-out 均已完成。当前可以认定服务端执行入口、durable replay、shared-Store fan-out、retention/archive 和 inbox 恢复路径已迁移，但 AF-CC1 仍不能标记 `[x]`。

### 0.5 统一 Event Subscription/Replay 行动进度

2026-08-13 已完成第一版统一事件订阅与重放：

- 新增 `EventStreamHub`、`EventSubscription`、`SubscribeResult` 与显式 `Event/Timeout/Closed/Overflow` 读取状态；
- durable `RuntimeEventEnvelope.sequence` 是唯一 cursor；ephemeral model/token/progress event 不写入 Store，也不进入 replay；
- subscribe 在 hub 临界区内完成 Store head/read 与 live subscriber 注册，发布端在同一 hub 锁下 fan-out，避免进程内 replay/live 间隙丢事件；
- subscription 按 sequence 去重；cursor 超过 head、replay digest/integrity 异常、replay 超过容量均 fail closed；
- 慢消费者队列满时关闭该 subscription 并返回 `Overflow`，不丢旧 durable event、不阻塞其他消费者；
- ConversationEngine 新增 `subscribe_events`，所有原子提交后的 durable event 同时 publish；
- AgentServer legacy/JSON-RPC SSE 均接受严格数值 `Last-Event-ID`，以 `runtime_event` 和 `id: <sequence>` 返回 replay/live event；cursor 超前返回 HTTP 409，低于 retention floor 返回 HTTP 410，并携带当前 head；
- `AgentClient::subscribe_task_runtime_events` 向 SDK 暴露 typed `RuntimeEventEnvelope` callback；`SSEConnection` 独立维护 runtime cursor，校验 envelope sequence 与 SSE `id` 一致并按 sequence 去重；
- `ConversationStore::event_retention_floor` 公开当前最早可 replay sequence；hub 对已淘汰 cursor 返回 `cursor_expired`，与 cursor 超前、完整性失败明确区分；
- 覆盖 live 顺序、cursor resume、重启 replay、容量拒绝、慢消费者 overflow 与 invalid cursor；针对测试 6/6、Phase 4 offline 74/74、Phase 3 22/22、五 demo 构建全部 PASS。

后续完成 shared-Store 跨进程 fan-out：subscription 在本地队列为空时按 cursor 有界拉取 Store，本地 publish 保持低延迟；两条路径按 sequence 有序去重，并以 `CursorExpired`、`IntegrityFailure`、`Overflow` fail closed。双 SQLite Store 及真实 `fork` 子进程写入测试已证明独立进程提交可被父订阅者读取。该模式适合共享 SQLite/WAL 的单机多进程；大规模多节点仍建议以 PostgreSQL LISTEN/NOTIFY 或消息总线替换轮询唤醒，但无需改变 SSE/SDK cursor 协议。

2026-08-13 进一步完成 CES-RAC0–RAC7：`conversation_event_streams` 将 head、retention floor、revision 和 archive chain head 持久化，事件序号不再依赖热表 `MAX(sequence)`；旧数据库由现存事件一次性回填，之后 head 即使热表删空也不回退。归档以确定性 v1 manifest 和连续 event range 写入内容寻址 ObjectStore，并通过 `prepared → upload → read-back verify → transactional prune` Saga 推进；上传失败保留 prepared payload，可在重启或再次调用时幂等恢复。数据库仅在对象 digest、manifest、事件 digest、sequence 连续性均验证通过后，原子删除热事件、推进 floor、提交 archive record。测试覆盖 dry-run、分批归档链、上传失败恢复、全量 prune 后 sequence 继续单调、旧 cursor 过期和归档回读校验。

### 0.6 长时间调用工具完成复杂工作的能力审计

这里必须区分三个不同成熟度，避免把“异步”误写成“可长期自主执行”：

| 能力层级 | 当前状态 | 判定 |
|---|---:|---|
| 异步调用一个工具 | `[x]` | ToolBus 返回 `future<json>`；只读工具和 A2A submit 可有界并行；Local/MCP/Skill 可接收协作式取消回调 |
| 持久恢复一个工具 effect | `[~]` | Tool Effect Journal 能记录 started/completed/committed、幂等键和 reconciliation policy；Durable Run 能识别 Prepared/Unknown effect；但 invocation 的进度、owner、lease、输出 cursor 和重新附着尚未统一 |
| LLM 长期驱动复杂工作 | `[△]` | Cognition、Harness、Remediation 和 Task Closure 可分阶段推理与验收，但尚无一个默认主路径让 LLM 在长工具运行期间执行 wait/observe/replan、消费部分结果、并行推进无依赖节点并在重启后继续 |

已存在的可复用基础如下：

- Cognition/Planning 已建模 critical path 以及 wall-time、token、tool-call、cost budget，并可持久化多阶段 LLM checkpoint；
- Harness/ProductionTaskRuntime 支持阶段 checkpoint、resume、remediation、reverification、progress ledger 和 bounded no-progress；
- Durable Queue 已实现 claim/renew、lease expiry、fencing token、retry/dead-letter、worker generation/heartbeat 和 tenant quota；
- ToolBus 已实现 schema、authorization、hook 改写后重新 authorization/schema validation、异步执行、有界只读并行及 `ToolCallControl`；
- Tool Effect Journal 已实现 durable WAL、idempotency key、Started/Completed/Committed/ManualReview/Failed/Cancelled 和恢复分类；
- Bubblewrap Sandbox 已实施 wall-time、CPU、memory、output 和 workspace 限额；MCP、Skill subprocess、LLM stream 各有部分取消路径；
- Conversation EventStream 已能持久 replay、跨进程 pull-through、归档和恢复，适合作为 invocation 低频 durable progress 的统一投影层。

仍缺的不是另一个工具函数，而是统一 Long-running Tool Invocation Kernel：

1. **durable invocation identity**：缺少统一 `invocation_id`、contract revision、owner、lease/fencing、attempt、deadline、budget、checkpoint ref、progress cursor、artifact/effect refs 与 terminal receipt；
2. **可恢复执行权威**：`future<json>` 只属于当前进程，进程退出后无法查询、重新附着或安全接管正在运行的 Local/MCP/HTTP invocation；
3. **完整状态机**：`ToolExecutionPhase` 目前只有 Started/Completed，无法表达 Queued、Leased、Running、Heartbeat、PartialResult、Checkpointed、Cancelling、Retrying、Reconciling、Orphaned 和 ManualReview；
4. **wait-observe-replan**：Agent 当前通常阻塞等待单个 future，缺少释放 Turn、事件唤醒、LLM 诊断进展、修改计划、推进其他 ready node 和恢复等待的 workflow；
5. **统一取消升级**：`ToolCallControl` 是协作式检查，未轮询的工具不会及时停止；Sandbox 主要依靠 timeout 后 SIGKILL，尚未统一 graceful cancel、grace period、强制 kill、MCP cancel、HTTP abort、A2A/ChildTask cancel 与 effect reconciliation；
6. **增量结果治理**：stdout、日志、partial artifacts、checkpoint 和大型 result 仍缺 ObjectStore-backed 分片、digest、cursor、retention 与 backpressure；
7. **跨 Store 原子关联**：Conversation、Run/Harness、Queue、Effect Journal 各有持久化，但没有 invocation coordinator 将其绑定为同一恢复事实；
8. **长时认证证据**：尚未验证数小时 soak、主进程/worker/MCP 重启、lease 接管、网络闪断、迟到 completion、重复通知、取消竞态、部分对象损坏和动态预算调整。

因此当前准确口径是：框架已经具备构建长期复杂工作流的多数控制面原语，但生产主路径仍是“短生命周期工具调用 + 阶段级 durable workflow”，尚不是“durable long-running tool workflow”。

## 1. 证据边界

### 1.1 Claude Code 不是原始 monorepo

所分析源码来自发布 bundle 的解包和 TypeScript 重建。部分 `feature()` 分支已被死代码消除，重建脚本还会关闭 feature gate 并为缺失模块生成 stub。因此，本报告只把以下可直接读出的机制作为架构证据：

- `QueryEngine` 的会话生命周期；
- `query()` 的显式状态循环；
- Tool 类型和动态工具池；
- validation、PreToolUse、permission、execution、PostToolUse 管线；
-流式 API、工具事件与取消；
-上下文预算、工具结果外置、compact boundary；
- JSONL transcript 与 parent UUID 链；
-子 Agent 独立上下文、权限、MCP 和 transcript；
-后台任务、通知队列和用户输入中断。

内部代号、未发布工具、线上 feature flag 值、服务端数据用途及未来模型路线不作为升级依据。

### 1.2 Agent Framework 使用当前事实，而非旧比较结论

当前 Agent Framework 已具备同轮只读工具并行、A2A submit 并行、Tool Effect Journal、Context Budget、Memory Compaction、Role Runtime、Cognition、Memory v2、Assurance、Remediation、Judge、Run/Harness/Approval Store、Bubblewrap、Credential Broker、OTLP/SLO、SQLite/PostgreSQL queue、lease/fencing、Object Store，以及 GPC 的 Task Closure、progress ledger、bounded stagnation 与 Golden Tasks。

GPW 已把完成语义接入 GraphExecutor、AgentServer、A2A、ChildTask 和多个 UI 入口；CTR 又新增 Conversation/Turn contracts、SQLite ConversationStore、状态机、事件 envelope、Context/Compact contracts 与 production bridge。因此，本报告不会重复建议“新增 TaskClosureController”“新增 ConversationEngine 骨架”或“增加只读工具并行”，而是从 GPC/GPW/CTR 之上继续完成交互执行内核的原子性、主路径接线和工具生命周期收敛。

## 2. 两个系统解决的层次不同

### 2.1 Claude Code：Conversation/Turn 中心

Claude Code 的核心单位是 conversation。一个 `QueryEngine` 持有跨 Turn 的消息、usage、权限拒绝、文件缓存、技能发现和 AbortController。每次 `submitMessage()` 启动一个 Turn，Turn 内 `queryLoop` 反复请求模型、执行工具、追加 tool result，直到自然结束、Hook 阻塞、预算耗尽、错误或取消。

它解决的是：

> 如何让一个交互式 Agent 在长会话、流式输出、权限询问、工具并发和上下文膨胀下持续工作？

### 2.2 Agent Framework：Run/Verified Task 中心

Phase 4 的核心单位是可恢复 Run/Harness。TaskIntake、Plan、AcceptanceContract、Approval、Artifact、Evidence、AcceptanceReport、Remediation 和 Closure Receipt 都拥有身份、revision 与 digest。

它解决的是：

> 如何证明一个复杂任务经过授权执行，产生了真实产物，并满足不可由模型自行降低的验收标准？

### 2.3 必须保留的双循环

```text
Conversation / Turn Loop
  User Input
    → Context Projection
    → Model Stream
    → Tool Lifecycle
    → Observation
    → ModelTurnOutcome

Task Closure Loop
  Task Intake
    → Contract / Plan
    → Approved Stage Execution
    → Artifact / Evidence
    → Assurance
    → Remediation / Reverification
    → TaskClosureDecision
```

边界必须严格：

- Turn Loop 输出 `ModelTurnOutcome`、Tool Effect Receipt、Observation、usage 和 context boundary；
- Closure Loop 输出下一阶段、能力、预算、澄清请求和任务终态；
- Turn Loop 永远不能签发 `completed_verified`；
- Closure Loop 不负责 token streaming、终端渲染或单工具动画。

## 3. 目标八层架构

```text
L7 Experience Plane
   CLI / Web / TUI / ImGui / SDK / A2A Gateway

L6 Conversation Plane
   ConversationEngine / TurnStateMachine / Stream Protocol / Input Queue

L5 Cognitive Plane
   RoleRuntime / Prompt / Skills / Planning / Memory View / AgentLoop Policy

L4 Capability Plane
   Tool Registry / Tool Lifecycle / MCP / A2A / Child Agent / Hooks

L3 Execution Plane
   Workflow / Taskflow / Sandbox / Artifact Executor / Streaming Executor

L2 Deterministic Control Plane
   ProductionTaskRuntime / Approval / Assurance / Remediation / Closure

L1 Durable Data Plane
   Run / Harness / Transcript / Effect / Artifact / Evidence / Memory Stores

L0 Governance and Operations Plane
   Policy / Identity / Audit / Telemetry / SLO / Eval / Live Certification
```

### L7：体验层

只负责呈现和用户动作，不持有隐藏完成逻辑。所有前端消费相同 typed events，不能根据输出文本、退出码或 spinner 推断任务完成。

### L6：会话层

该层已从最小骨架推进到单进程 durable conversation runtime：ConversationEngine、TurnStateMachine、Store、durable inbox、统一 event replay/live subscription 和入口适配已形成。下一步集中补齐跨进程事件通知、retention/archival、运行中取消传播和模型 ContextProjection，而不是重复建设入口与输入队列。

### L5：认知层

LLM 提出理解、计划、工具调用和解释。Memory View、Skill snapshot、Prompt/Profile revision 必须 pin；LLM 不拥有权限、effect commit 或任务完成权。

### L4：能力层

内建工具、MCP、A2A、Skill script 和子 Agent 统一经过 Tool Lifecycle Kernel，禁止执行旁路。

### L3：执行层

Taskflow/Workflow 提供依赖调度，Sandbox 和 Artifact Executor 执行副作用。该层只返回 receipt，不判断任务完成。

### L2：确定性控制层

继续发挥 Agent Framework 的优势：Approval、Assurance、Remediation、Reverification 和 Closure 必须确定性、可恢复、fail-closed。

### L1：持久数据层

严格区分 transcript、workflow event、external effect、artifact/evidence 和 memory，避免把所有状态塞进 session history 或一个 checkpoint。

### L0：治理与运维层

Policy、身份、审计、trace、cost、SLO、Eval 和 Certification 横切所有层，但通过统一 envelope 关联，不把业务模块变成遥测字段拼装器。

## 4. 升级方向一：ConversationEngine

### 原因

`AgentLoopNode` 适合嵌入 Workflow 图，但目前仍承载 Prompt、Memory、LLM、工具、压缩和回合退出。不同 UI/Server 又各自管理会话和流式状态，产生以下问题：

-入口之间可能形成不同会话语义；
- model-turn 和 task-terminal 容易混淆；
-压缩、取消、用户输入插队难以统一复用；
-生产 Harness 强，但轻量聊天路径边界不清；
-Run 恢复强于面向用户的 Conversation 恢复。

### 建议接口

```cpp
struct TurnRequest {
  ConversationId conversation_id;
  TurnId turn_id;
  UserInput input;
  ExecutionTrustProfile trust_profile;
  ContextProjectionRef context;
  CapabilityGrantSet grants;
  TurnBudget budget;
};

struct TurnState {
  std::vector<MessageRef> messages;
  std::optional<CompactBoundaryRef> boundary;
  TurnContinuationReason continuation;
  UsageSnapshot usage;
  std::uint64_t iteration;
};

struct ModelTurnOutcome {
  ModelTurnStopReason reason;
  std::string candidate_answer;
  std::vector<ToolCallReceiptRef> tool_receipts;
  std::optional<ClarificationRequest> clarification;
  bool task_completion_verified{false};
};
```

`ConversationEngine` 应提供 `start_turn`、`continue_turn`、`interrupt_turn`、`resume_turn`、`submit_user_input`、`subscribe_events` 和 `project_context`。

### continuation 与 terminal 必须分离

Continuation 至少包括：`InitialRequest`、`ToolResultsAvailable`、`QueuedUserInput`、`StopHookBlocked`、`ContextCompacted`、`PromptTooLongRecovery`、`OutputTokenRecovery`、`BudgetContinuation`、`ReplanRequested`、`ClarificationAnswered`、`ResumeAfterApproval`。

Model stop 至少包括：`EndTurn`、`ToolRequested`、`GuardStopped`、`ProviderError`、`Cancelled`、`DeadlineExceeded`、`ContextExhausted`、`MaxIterations`。它们全部映射到 `task_completion_verified=false`。

### 当前落点与剩余接线

- `[x]` 已新增 `include/agent/conversation/` 与 `src/conversation/`，形成 types/store/state-machine/engine/context/bridge/adapter；
- `[x]` 已建立 SQLite ConversationStore、Turn CAS、parent/digest chain、durable event 与 compact boundary；
- `[x]` legacy Graph execution 经 `GraphTurnAdapter` 后只能产生 unverified model outcome；
- `[x]` 五个 demo 已共享 LiveRuntime/profile bootstrap；四个交互 demo 经公共 Conversation adapter，legacy `run_react_cli_sync` 位于受控 TurnExecutor callback 后；
- `[ ]` 让 `AgentLoopNode` 适配 TurnStateMachine 并逐步瘦身；
- `[~]` CLI/Web/TUI/ImGui、SDK、AgentServer/A2A 已接入 Conversation execution/event boundary，但尚未全部只依赖一个原生 Conversation API；
- `[x]` durable input inbox、跨 Turn FIFO 消费、event subscribe/replay、SDK cursor 和断线续传已实现；
- `[ ]` 跨进程 event fan-out、provider/tool cancellation propagation、Conversation retention/compact 与真实 ContextProjection invocation 尚未实现。

## 5. 升级方向二：Tool Lifecycle Kernel

### 原因

当前已有 ToolBus、schema、effect journal、approval、sandbox、Skill policy 和 observer，但能力分散在不同路径。Claude Code 最值得借鉴的不是某条 Bash 正则，而是统一工具生命周期：

```text
Resolve → Parse → Validate → Pre Hook → Policy → Approval
→ Sandbox/Execute → Output Validate → Effect Commit → Post Hook → Observe
```

### 版本化 Tool Contract

```cpp
struct ToolContract {
  ToolIdentity identity;
  JsonSchema input_schema;
  JsonSchema output_schema;
  ToolEffectClass effect;
  ConcurrencyClass concurrency;
  InterruptBehavior interrupt_behavior;
  ResultRetentionPolicy retention;
  SandboxRequirement sandbox;
  ApprovalRequirement approval;
  CapabilitySet capabilities;
  TelemetryPolicy telemetry;
};
```

生产默认必须保守：

-未声明 effect 按有副作用处理；
-未声明 concurrency 按串行处理；
-未声明 sandbox 的写操作拒绝；
- destructive 必须显式声明；
-输入改写产生新 digest，原始模型输入保留；
-Tool Contract revision 进入 Run/Invocation manifest。

### 固定执行顺序

1. schema parse；
2. deterministic normalization；
3. tool-specific validation；
4. PreToolUse advisory hook；
5. Policy Decision Point；
6. Approval resolution；
7. sandbox binding；
8. effect reservation/idempotency lookup；
9. execution；
10. output validation；
11. effect commit/reconciliation；
12. PostToolUse hook；
13. retention/context projection。

Hook 不能绕过 policy；修改输入后必须重新 validation 和 policy。未知外部 effect 进入 `ManualReview`。

### 大结果治理

使用现有 ObjectStore/CAS：小结果内联，大结果保存为 content-addressed artifact，模型只获得 preview、URI、digest、MIME、size 和读取方法。压缩只替换 Context Projection，不删除原始 effect/evidence；所有裁剪生成 `TruncationReceipt`。

## 6. 升级方向三：统一流式事件协议

### 原因

Claude Code 能同时服务 REPL、print、SDK 和远程模式，关键是持续产生结构化事件。Agent Framework 现已建立 `RuntimeEventEnvelope`、durable sequence cursor、Store replay、进程内 live fan-out、SSE `Last-Event-ID` 和 SDK typed callback；剩余问题是扩大事件覆盖面、跨进程通知和 retention/archival，而不是重新定义基本订阅协议。

```cpp
struct RuntimeEventEnvelope {
  EventId event_id;
  TraceContext trace;
  TenantId tenant_id;
  ConversationId conversation_id;
  TurnId turn_id;
  RunId run_id;
  std::optional<NodeAttemptId> node_attempt;
  std::optional<ToolCallId> tool_call_id;
  EventSequence sequence;
  EventDurability durability;
  Visibility visibility;
  RedactionClass redaction;
  RuntimeEvent payload;
};
```

事件应覆盖 model request/stream/usage/stop，tool queued/permission/started/progress/completed，user input queued/consumed，compact boundary，Harness transition，approval，artifact/evidence 和 closure。

Token delta、spinner 属于 ephemeral；approval、effect、boundary、checkpoint、closure 属于 durable。高频 progress 不得进入恢复消息链。

## 7. 升级方向四：Conversation、Workflow、Effect 三链分离

```text
Conversation Chain
  用户/助手/工具可见语义，支持 branch、resume、compact projection

Workflow Event Chain
  stage/node/attempt/checkpoint/approval/closure 状态变化

Effect Chain
  副作用 reservation、started、observed、committed、unknown、reconciled
```

建议新增 append-only `ConversationStore`：消息具有 `message_id`、`parent_id`、`turn_id`；compact boundary 是一等记录；progress 不参与 parent chain；tool result 引用 effect receipt；candidate answer 引用 model invocation；Conversation Store 不拥有任务终态。

恢复时分别重建 Conversation projection、Run/Harness cursor、未决 approval、effect reconciliation queue 以及 pinned Memory/Skill/Prompt/Profile。摘要或 revision 漂移必须进入 ManualReview。

## 8. 升级方向五：Context Projection 与 Compact Boundary

### 原因

Memory v2 治理长期事实，compact boundary 治理长对话，两者不是替代关系。每次模型调用应使用不可变 `ContextProjectionManifest`：

```text
System/Policy reserved
Task Contract/Plan reserved
Working turn
Recent conversation
Relevant Memory View
Tool/Skill definitions
Artifact/Evidence previews
```

每段记录来源 revision/digest、authority、freshness、sensitivity、token budget、裁剪理由、外置 URI 及是否可用于验证。

`CompactBoundaryRecord` 应记录 pre/post token、summary digest、preserved head/tail、归档范围、profile/prompt/model、fallback 原因和一致性检查。压缩失败不得无限重试；一次 reactive compact 后仍超长，应进入 `ContextExhausted`。

压缩顺序建议：工具结果外置 → 确定性去重 → 旧 tool group 结构化摘要 → 对话 compact → Memory consolidation → 最后 UTF-8 安全截断。Contract、Policy、Approval、Citation 不得静默裁剪。

## 9. 升级方向六：子 Agent 与任务隔离

子 Agent 必须重新计算工具、MCP、filesystem/network/sandbox grants、Memory View、模型/profile、预算、交互权限和 transcript namespace，而不是复制父 Agent 全部授权。

```text
effective_grants =
  parent_delegable_grants
  ∩ child_role_capabilities
  ∩ task_contract_capabilities
  ∩ policy_decision
  ∩ deployment_manifest
```

父任务只能接收子 Agent 的候选 Artifact、Evidence、Finding、Investigation result 和 execution receipt，不能根据子 Agent 自报 completed 推出父任务完成。

子任务状态应细化为：

```text
Pending → Admitted → Running → AwaitingInput/Approval
→ Reconciling → SucceededCandidate/Failed/Cancelled/UnknownEffect
```

A2A peer 的 completed 只是 assertion；生产端必须校验身份与 digest，并本地重做 mandatory strong oracle。无法观测的远端副作用进入 ManualReview。

## 10. 升级方向七：流式工具执行与依赖安全调度

建议把工具从只读/写二分升级为：`PureRead`、`SnapshotRead`、`WorkspaceWrite`、`ExternalIdempotentWrite`、`ExternalNonIdempotentWrite`、`Interactive`、`Barrier`。

- PureRead 可在相同 snapshot 并发；
- SnapshotRead 必须 pin generation；
- workspace write 依据 path/effect domain 建冲突边；
-外部幂等写需要 reservation key；
-非幂等写默认串行并需 Approval；
- Interactive/Barrier 阻断同批后续执行。

不应复制 JavaScript Promise 分批器，而应把工具批次编译成临时 Taskflow subflow：自动建立冲突边、并发读、串行写、按原 tool-call 顺序组装结果，并观测 critical path、queue time 和 blocked-on-approval。

流式提前执行只有在 tool name/schema 输入完整、Policy/Approval 完成、取消策略明确时允许。流中的后续文本不能修改已签发输入。

## 11. 升级方向八：权限、Sandbox 与 Approval 统一

Claude Code 的 Bash 字符串分析是产品折中，不应成为 C++ 框架的安全根基。Agent Framework 应坚持：

-Policy/Approval 决定是否允许；
-Sandbox 决定实际上能做什么；
-Effect Journal 决定做过什么以及是否确定；
-Oracle 决定结果是否满足验收。

Deny precedence 应固定为：Platform hard deny > Organization deny > Task Contract deny > Ask requirement > Scoped allow > production default deny。

为了减少反复询问，审批应绑定 action set，而不是永远逐调用询问：绑定参数范围、workspace revision、最大 effect count、有效期和风险等级；参数变化、scope 扩张或风险升级自动生成新 request。

## 12. 升级方向九：动态工具、Skills 与 MCP 渐进披露

工具数量增长会增加 Prompt、选择错误率、权限面和缓存失效率。应把 Skills L1/L2、ToolBus 与 MCP refresh 统一为 Capability Discovery。

Capability Catalog 只保存 capability id、search hint、effect/risk、permission、schema digest、source、health 和 generation。首轮只暴露核心工具、always-load 工具和 Capability Search；命中后 pin generation，再加载完整 schema、Prompt 与 Policy。

状态必须分开：

```text
Discovered → Resolved → Verified → Granted → Loaded → Invoked
```

动态 MCP refresh 只能在 Turn 边界发生，避免同一模型请求内工具定义漂移。

## 13. 升级方向十：用户输入中断与后台通知

用户在工具运行时的新输入需要分类：`InterruptAndReplace`、`AppendToCurrentTurn`、`QueueNextTurn`、`ControlAction`、`StatusQuery`。

工具声明中断行为：

- cancel：立即取消并丢弃未提交结果；
- block：继续执行，新输入排队；
- reconcile：取消后查询外部 effect；
- non-interruptible：运行到安全 checkpoint。

后台任务完成不应靠主 Agent 高频轮询。Notification Queue 在 Turn 边界或显式 wait 时注入，携带 task/agent/run identity，具备 idempotency，并严格按父 Agent 路由。通知只是 observation，不是 completion evidence；Slash command 不能作为普通模型文本静默注入。

### 长时工具的等待、观察与重规划

复杂工作不能通过让一个 C++ future 阻塞数小时来实现，也不能让 LLM 每隔数秒查询一次状态。目标执行模型应为：

```text
Plan ready node
  → reserve effect / create durable invocation
  → enqueue + lease + start adapter
  → persist progress/checkpoint/partial artifact
  → release current Turn
  → event/notification wakes orchestration
  → deterministic progress gate
       ├─ routine heartbeat: update state, no LLM
       ├─ useful partial result: project bounded observation to LLM
       ├─ anomaly/stall: LLM diagnose and revise plan
       ├─ approval/clarification: durable interruption
       └─ terminal receipt: verify effect/artifact and unblock dependents
```

LLM 介入必须由事件语义和信息增益驱动：新证据、异常、预算偏差、依赖变化、验收失败或显式决策点才触发 cognition/replanning。普通 heartbeat、重复百分比和未变化日志由确定性控制器聚合，避免 token 浪费和“轮询即思考”的伪自主性。

每个长时 invocation 至少需要以下状态：

```text
Created → Admitted → Queued → Leased → Running
  → Progressing / Checkpointed / AwaitingInput / AwaitingApproval
  → Cancelling / Retrying / Reconciling
  → CompletedCandidate / Failed / Cancelled / Orphaned / ManualReview
  → EffectCommitted / Verified
```

`CompletedCandidate` 只表示 adapter 返回结果；只有 effect receipt、artifact digest 和强 oracle 均闭合后，才允许进入 `EffectCommitted/Verified`，并由 Task Closure 判断任务级完成。

## 14. 升级方向十一：快速路径与专业路径分级

所有请求走完整 Harness 会导致简单问答过重；所有请求走 AgentLoop 又缺乏验收。应选择类型化 profile：

| Profile | 用途 | 必需控制 |
|---|---|---|
| Conversation | 无副作用问答 | Conversation Store、budget、model-turn outcome |
| ReadOnlyAnalysis | 仓库/数据分析 | citations、只读 sandbox、轻量 contract |
| ArtifactDelivery | 文档/文件交付 |路径、digest、内容 oracle、Closure |
| CodeChange |代码修改 | Approval、diff、build/test、remediation |
| ExternalAction |消息、部署、远程变更 | accountable approval、effect reconciliation |
| Professional |科研/安全/生产验收 |完整 Cognition、Memory、Assurance、Judge |

发现副作用只能升级 profile；删除 mandatory criterion 或 profile 降级必须经过 Policy/必要 HITL；生产入口缺少 profile 时 fail closed。

## 15. 升级方向十二：Operations 与开发者体验

四端 UI/API 应从相同 `OperationsSnapshot` 投影 Conversation/Turn、profile、model stop、Harness stage、approval、tool queue、criteria coverage、artifact/evidence、closure authority、budget/cost 和可用动作。

面向用户可以归并为 Thinking、Waiting for permission、Running tools、Verifying、Needs input、Blocked externally、Completed and verified、Stopped with limitations、Manual review，但必须允许展开查看 reason、authority、receipt 和未满足 criteria。

建议提供 redacted diagnostic bundle：manifest revisions、event sequence、context projection summary、tool/approval/effect receipts、closure decision、trace 与 SLO；不包含 secret 和私有 chain-of-thought。

## 16. 不应照搬 Claude Code 的设计

1. **不复制巨型 query loop**：显式状态值得学习，但应用状态、UI、遥测和策略应拆成状态机与 ports。
2. **不把 Tool 变成万能接口**：Capability、Execution Adapter、Policy Metadata、Presentation Adapter 应分离。
3. **不依赖 shell parser 提供强安全**：字符串分类只用于 UX，强边界依赖 sandbox 和 credential isolation。
4. **不让 feature flag 无审计改变生产语义**：影响权限、完成、证据的 flag 必须 pin 到 deployment manifest。
5. **不以 transcript 代替 durable workflow**：对话可恢复不等于 effect 可恢复。
6. **不保存私有 chain-of-thought**：保存结构化理由、证据、假设、计划和决策摘要。

## 17. 分阶段实施路线图

以下工作位于 GPC/GPW 之上。

### AF-CC0：契约冻结 `[~]`

冻结 Conversation、Turn、RuntimeEvent、ToolContract、ContextProjection 和 CompactBoundary schema。未知 version fail closed；model-turn 与 task-terminal 在类型系统中不可互换；所有 profile/feature revision 进入 deployment manifest。

### AF-CC1：ConversationEngine 生产闭环 `[~]`

事务化 Turn boundary、跨连接 sequence/parent linearizability、durable inbox/FIFO 跨 Turn 消费、subscribe/replay、shared-Store 跨进程 fan-out、retention/archive/compaction、SDK typed cursor、100 Turn 消息链、重启恢复和入口适配已完成。下一退出条件是：通过 LTW8 完成运行中取消传播，让 Conversation ContextProjection 驱动真实 invocation，以及关闭 crash/busy/disk/schema-migration mandatory matrix。

### AF-CC2：Tool Lifecycle Kernel `[△]`

Local/MCP/Skill/A2A/Artifact tools 统一进入生命周期。验证 Hook 改写后重新校验、deny precedence、未知 effect 拒绝、幂等不重复、unknown effect ManualReview、大结果 CAS 外置。

### AF-CC3：Context Projection/Compact Boundary `[~]`

既有 Memory Compaction 已覆盖自动/manual trigger、隔离 compaction LLM 和 fallback/cancel/timeout；本工作包剩余目标是把这些机制收敛到 Conversation ContextProjection/CompactBoundary，要求 Contract/Policy/Citation 不丢失、compact 前后恢复等价、reactive compact 有界、summary failure 有确定性 fallback、长会话 RSS/token 成本下降。

### AF-CC4：三链持久化与恢复 `[~]`

Conversation、Workflow Event、Effect 链已有独立 Store，Run-Harness saga、CrossStoreCoordinationJournal 与 Effect reconciliation 已存在。剩余是把 Conversation 纳入 correlation/coordination，并对所有 durable 写点做 crash injection，确保无 orphan、无 effect 重放、digest 漂移转 ManualReview。

### AF-CC5：子 Agent Capability Isolation `[△]`

ChildTask/A2A/Skill 子运行时使用能力交集和独立 transcript。父授权不泄漏、子 Agent 无法扩权、父取消传播、子自报完成不能关闭父任务、远端证据本地复验。

### AF-CC6：Streaming 与安全调度 `[△]`

用 Taskflow subflow 构建工具批次。PureRead 并发应获得可量化延迟收益；冲突写永不并发；结果顺序符合 provider 协议；stream abort 不遗留未协调 effect；critical path 可观测。

### AF-CC7：统一 Experience/Operations `[~]`

四个交互 demo 已经公共 Conversation adapter，SDK/AgentServer/A2A 已消费 typed runtime event 与 cursor，candidate/verified 区分已存在。剩余退出条件是所有端统一 Operations snapshot/action contract、真实 ApprovalStore 动作、取消/后台通知一致且跨进程断线重连不回退。

### AF-CC8：质量、故障与生产认证 `[~]`

把新交互运行时纳入 unit、contract、integration、recovery、adversarial、performance 和 live-production 七层测试。真实 provider/MCP/IdP/KMS/Sandbox 证据必须由现有 Live Certification 签发，不能 skip-as-pass。

### AF-CC-LTW：Long-running Tool Workflow `[~]`

该工作包是当前复杂任务自主执行能力的主线，按依赖顺序实施：

1. **LTW0 — 契约与不变量 `[x]`**：冻结 `LongRunningToolInvocation`、`InvocationEvent`、`ProgressCheckpoint`、`PartialResultRef`、`InvocationReceipt` schema；明确 invocation terminal 与 task terminal 不可互换；未知 effect、丢失 fencing 或 revision 漂移一律 fail closed。
2. **LTW1 — Durable Invocation Store `[x]`**：实现 SQLite production baseline，保存 CAS revision、状态、attempt、owner、lease/fencing、deadline/budget、input digest、tool/deployment generation、progress cursor、checkpoint、artifact/effect refs 和 append-only event；支持 bounded replay、按 tenant/conversation/run/tool 查询、组件私有 schema version/migration 和私有文件权限；可与 Queue/Worker 表安全共存于同一数据库。
3. **LTW2 — Lease Worker Runtime `[x]`**：将 Durable Queue/Worker Registry 组合为默认 invocation scheduler，提供 claim、周期 renew、heartbeat、expired takeover、tenant quota、retry/dead-letter；旧 owner 的迟到写入必须被 fencing 拒绝。
4. **LTW3 — Progress/Streaming Protocol `[x]`**：已完成 invocation durable event head/floor/cursor/bounded replay、同进程 publish 唤醒与跨 Store 实例追赶、checkpoint/partial-result 引用、Conversation runtime-event 投影，以及 heartbeat/information-gain 分类；慢消费者采用有界缓冲并显式 `Overflow` 后从 durable cursor 恢复，事件缺口、过期 cursor 和完整性失败均 fail closed；retention 在删除前写入 tenant-scoped ObjectStore archive，CAS 推进 retention floor 并保留 digest anchor、terminal receipt、最新 checkpoint 和 partial/artifact 审计引用。
5. **LTW4 — Typed Execution Adapters `[~]`**：已实现 production origin gate 和版本/部署代次 pin，Local/ToolBus、Bubblewrap、MCP(kind)、HTTP/remote API、A2A/ChildTask 均通过统一 start/attach/query/cancel/reconcile contract 声明真实 capability 与 restart policy；Worker 在启动外部操作后原子持久化 adapter identity、external operation id 和 fencing，并验证 HTTP 重启 attach。仍需 Bubblewrap 进程组 grace/kill、MCP 原生 session attach 和 A2A 远端 task id 持久化，当前不支持的能力保持显式 false/ManualReview，不得伪装可恢复。
6. **LTW5 — Effect/Artifact Commit Coordination `[~]`**：已实现 `InvocationCommitCoordinator`，关联 Invocation Store、Tool Effect Journal、ObjectStore、RunStore 和 CrossStoreCoordinator；候选输出计算 canonical digest、对象写入后回读验证、effect Prepared→Committed、Run effect commit、receipt upsert 与 invocation EffectCommitted；Unknown 非幂等 effect 创建 journal record 后强制 ManualReview，重复提交复用稳定 operation/idempotency key。仍需真实 Harness Store participant 的 production wiring、输入 reservation 独立阶段、所有写点 crash injection 与 orphan sweeper，故暂不标记完成。
7. **LTW6 — LLM wait-observe-replan Workflow `[x]`**：独立 `LongTaskWorkflow`/SQLite Store 持久化 workflow/node/watch/budget/plan pin/cognition manifest，checkpoint CAS 与 append-only digest event 同事务提交并拒绝 stale fencing owner。确定性 classifier 确保 routine heartbeat 只推进 cursor、不调用 LLM；InvocationEventSubscription 与 RunStore durable timer 分别提供事件/timeout 唤醒。ready-DAG scheduler 输出可并行节点；类型化 `PlanNodeExecutionDescriptor` 由 durable ProductionWorkflowInputRepository 提供，不解析 objective 文本；production-origin executor registry、adapter executor 和 dispatcher 完成 capability/approval/digest gate、幂等启动、Invocation watch 持久化与 Invocation→node reconciliation。`RoleRuntimeLongTaskModel` 使用 pinned profile/prompt/memory view，LLM revision proposal 经完成/effect 保护、预算上限、DAG validation 后才可 PlanStore CAS。默认 production dependency validation 缺 LongTaskStore、InvocationStore、descriptor repository、executor registry、workflow 或 timer worker 任一项均 fail closed。测试覆盖 1000 heartbeat 零 LLM、meaningful wake 去重、restart cursor、digest/fencing、DAG unlock、typed dispatch 去重、durable descriptor reload 与 timer claim/complete。
8. **LTW7 — Incremental Result/ObjectStore `[~]`**：新增 tenant-scoped `IncrementalResultStore`/SQLite durable registry，在不可变 ObjectStore CAS 上实现 stdout/stderr/log/partial/checkpoint/artifact 确定性分片、revision/parent manifest、append 幂等、head CAS、seal/abort、重启恢复、内容去重和 chunk/manifest 完整性验证。分片前 redaction 保证匹配规则的原文不进入 CAS；bounded result-view assembler 只向 LLM 提供严格字节预算的脱敏 preview、manifest digest 与读取引用，并已接入 `RoleRuntimeLongTaskModel`。Worker observation 可写入真实 stream 并以 canonical `PartialResultRef` 关联 Invocation event；production dependencies 缺少 incremental store 时 fail closed。SQLite 对象索引支持 orphan 标记，审计与 metrics 覆盖写入、冲突、脱敏、截断、完整性和孤儿对象。测试覆盖多分片、去重、租户隔离、CAS、seal、重启、预览预算、secret 不泄漏及相关执行链回归。残余：底层 ObjectStore 尚无安全 delete/list contract，因此 physical GC、归档迁移与 legal-hold 后最终删除尚未闭环；跨多次 append 的规则匹配需由上游保持语义记录完整，真正 stateful streaming redactor 仍待补齐。
9. **LTW8 — Cancellation/Deadline Closure**：将 Conversation Interrupt、用户 cancel、Harness/Run deadline 贯通到 scheduler、provider、tool、sandbox、MCP、HTTP、A2A 和 child task；执行 cooperative cancel → grace period → force terminate → reconcile，并持久化 cancel request/ack/kill/effect 状态证据。
10. **LTW9 — Recovery/Soak Certification**：覆盖进程/worker/MCP 重启、lease takeover、网络分区、失联后恢复、重复/迟到 completion、对象损坏、取消竞态、磁盘满、busy timeout 和 schema migration；完成至少数小时 soak 与真实 provider/tool Live Certification。

建议初期只以 SQLite + 本机多进程作为 correctness baseline；跨主机扩展复用同一 invocation/event contract，替换为 PostgreSQL queue/notification 或消息总线。不要在单机恢复、fencing 和 effect reconciliation 尚未闭环前引入新的分布式执行后端。

## 18. 定量退出门槛

### 正确性与安全

| 指标 | 门槛 |
|---|---:|
| False verified completion | 0 |
| 未授权 effect | 0 |
| 已确认 effect 重放 | 0 |
| Conversation parent-chain orphan | 0 |
| compact 后 mandatory citation 丢失 | 0 |
| 子 Agent 权限扩张 | 0 |
|远端自报 completed 直接关闭本地任务 | 0 |

### 收敛效率

| 指标 | 目标 |
|---|---:|
|无信息增益自由 Turn | ≤1 |
|平均修复轮数 | ≤1.5 |
|每关闭一个 criterion 的 token/tool 成本 |持续下降 |
|可并发只读工具 critical-path 降幅 | ≥25% 基线目标 |
| Conversation profile 额外 Harness 延迟 | 近零 |

### 可恢复性与体验

| 指标 | 门槛 |
|---|---:|
| durable boundary crash 恢复率 | 100% mandatory matrix |
| unknown effect 自动误判成功 | 0 |
|重复 notification 造成重复动作 | 0 |
| Store digest 漂移静默继续 | 0 |
|未验证回答显示 verified badge | 0 |
|四端 Operations 状态不一致 | 0 |

### Long-running Tool Workflow

| 指标 | 门槛 |
|---|---:|
| invocation durable transition CAS 冲突静默覆盖 | 0 |
| lease 丢失后旧 worker 提交成功 | 0 |
| 非幂等 unknown effect 自动重放 | 0 |
| restart 后不可分类 invocation | 0 |
| cancelled invocation 继续产生未协调 effect | 0 |
| partial artifact digest/sequence 缺口静默接受 | 0 |
| routine heartbeat 触发 LLM 调用 | 0 |
| 无信息增益 progress 导致 plan revision | 0 |
| supported adapter 的 cancel/deadline 传播覆盖率 | 100% mandatory matrix |
| durable invocation crash 恢复/接管率 | 100% mandatory matrix |
| 数小时 soak 中 event sequence、fencing、effect 重复错误 | 0 |

## 19. 优先级建议

已完成且不应重复规划：ConversationStore 原子 Turn boundary、跨连接 sequence/parent linearizability、durable inbox 与 queued input 跨 Turn 消费、四端/Server 入口适配、统一 Event Subscription/Replay、shared-Store 跨进程 fan-out、内容寻址 event retention/archive/compaction、SDK typed cursor。

当前最短技术缺口依赖顺序：

1. **AF-CC-LTW0–LTW3**：先冻结 durable invocation contract，完成 Invocation Store、lease/fencing worker 和 progress/checkpoint/event 协议；否则长时执行仍只是不可恢复的进程内 future；
2. **AF-CC-LTW4–LTW5**：实现 typed adapters，并把 invocation、effect、artifact 与 Run/Harness 关联；非幂等 unknown effect 必须进入 reconciliation/ManualReview；
3. **AF-CC-LTW6**：实现 LLM 驱动但非 LLM 轮询的 wait-observe-replan workflow，使复杂任务能够在工具运行期间释放 Turn、接收事件、并行推进和基于新证据修订计划；
4. **AF-CC-LTW7R–LTW8**：补齐增量结果的 stateful 跨-append redaction、ObjectStore 安全枚举/删除、physical GC/归档/retention，再完成 backpressure 以及 provider/tool/sandbox/MCP/A2A/child task 的 deadline/cancel/kill/reconcile 闭环；
5. **AF-CC3**：让不可变 ContextProjectionManifest 驱动真实模型 invocation，并把 long-running partial result 以 bounded projection 纳入上下文；
6. **AF-CC-LTW9/AF-CC8**：执行真实 crash/takeover/network/cancel/late-result/soak mandatory matrix 和 Live Certification；
7. **AF-CC2/4/5/6/7**：在 LTW 内同步收敛 ToolContract、CrossStore coordination、子 Agent capability intersection、effect-aware Taskflow 调度和 Operations 展示；
8. **规模化增强**：单机 durable correctness 通过后，再以 PostgreSQL notification/消息总线替换 shared SQLite polling/queue backend，保持 invocation sequence、fencing、cursor 和 receipt 契约。

在 LTW0–LTW5 和 AF-CC4 关闭前，冻结无法声明 attach/cancel/reconcile 语义的新 provider adapter、新 Store 类型、绕过 Invocation/Effect 生命周期的工具系统、平行 demo runtime，以及只有 fixture 没有 production wiring 的控制面。

## 20. 最终目标

升级后的 Agent Framework 不应被描述为“C++ 版 Claude Code”，而应定位为：

> 一个具有 Claude Code 级交互式执行能力、Taskflow 级并行调度能力，以及强于普通 coding agent 的确定性验收、durable recovery、专业 Assurance 和生产认证能力的 Agent Operating Runtime。

最终主路径：

```text
User / SDK / A2A
  → ConversationEngine
  → ContextProjection
  → Cognitive Turn
  → Tool Lifecycle Kernel
  → Taskflow/Sandbox Execution
  → Artifact/Effect/Evidence Receipts
  → ProductionTaskRuntime
  → Assurance / Minimal Remediation / Reverification
  → TaskClosureController
  → Verified Operations Projection
```

该架构明确回答四个通常被混淆的问题：模型是否结束本轮表达；工具是否被安全、正确、幂等执行；会话是否能跨压缩、崩溃和分支继续；任务是否由强证据证明完成。

Claude Code 提供了前三项的大量成熟产品经验；Agent Framework 已为第四项建立了难得的确定性基础。真正有价值的升级不是扩大功能清单，而是通过 Conversation、Tool、Context、Event 与 Closure 五个权威边界，把现有能力收敛成一条低延迟、不可绕过、可恢复且可验证的生产路径。
