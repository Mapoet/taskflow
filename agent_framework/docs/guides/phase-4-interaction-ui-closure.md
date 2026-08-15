# Phase 4 Interaction Graph 与对话联动 UI 闭环计划

**计划版本**：`phase4-uix-r2`
**状态**：residual-local-closed / external-partial；UIXR0–UIXR8 已于 2026-08-15 连续实施并通过本地认证，真实 IdP/KMS 与跨主机 Live 认证保持开放
**批准**：用户于 2026-08-15 批准落盘 `P4-UIX0 → P4-UIX9` 详细计划
**上游章程**：[Phase 4](./phase-4.md)
**事实基线**：[Phase 4 状态与证据矩阵](./phase-4-status.md)
**生产闭环**：[Phase 4 v2 Residual Closure](./phase-4-residual-closure-plan.md)
**相关契约**：`phase4.operations.v1`、Conversation/Turn、AgentTemplate、Memory v2、Approval、Assurance、Tool Runtime

## 1. 目标与非目标

本计划不再增加互相孤立的 Planner、Memory、Agent、Approval 页面，而是建立一个以
`Conversation → Turn → Task/Run` 为主轴的 **Interaction Graph**。用户在对话中点击某个语义对象时，
UI 必须定位到同一次运行、同一 revision 和同一证据闭包中的专业过程；控制面中的对象也必须能反向返回
其来源消息。

本计划将 Planner 区域定义为四个联动检查器：

1. **Think & Plan**：可展示的思考摘要、认知/调查过程、假设、未知项、计划 DAG 和 revision；
2. **Memory**：本轮实际选择的五层 Memory View 及选择/排除理由；
3. **Agents**：父 Agent、子 Agent、协同 Agent、Skill/runner、工具、进度和阻塞；
4. **Approval**：Agent 原问题、审批依据、风险、动作、身份、多签进度和恢复目标。

Assurance、Evidence、Tool、Artifact 作为四个检查器的关联对象和二级视图。系统不得展示模型私有
chain-of-thought；Think & Plan 只消费允许展示的 reasoning summary、结构化认知事实、证据和决策理由。

非目标：

- 不用 DOM 文本、数组下标或命名约定猜测跨模块关联；
- 不将 Operations demo snapshot 当作生产事实；
- 不将 UI 内存状态当作 durable source；
- 不因本计划实施而放宽 Memory、credential、prompt 或 reasoning 的可见性策略；
- 不用单页“能显示”替代重启、replay、身份和 revision 一致性。

## 2. 当前审计结论

### 2.1 总体判断

当前框架已经具备 Conversation、Harness、Cognition/Planning、Memory、AgentTemplate、ChildTask、Tool/LTW、
Approval、Assurance/Judge、TaskClosure 和 Operations projection，但它们只形成了局部生产接线。当前
`phase4.operations.v1` 能展示阶段、摘要、Memory item、LLM invocation、AgentTemplate、SkillNode 和 HITL；
Web Conversation 能展示消息、thinking summary、工具和 artifact。两者之间只有工作区级切换，没有对象级
双向关联。

因此当前状态是：**模块基本齐全、主流程局部接通、身份部分共享，但尚无统一 Interaction Graph，不能宣称
已经形成对话可观察、可导航、可控制的综合完整 Agent 框架。**

### 2.2 接线矩阵

| 链路 | 当前状态 | 已有事实 | Mandatory 缺口 |
|---|---:|---|---|
| Conversation → Harness | `[~]` | harness turn adapter、execution-path event | `turn_id` 被隐式复用为 task/run；无正式 binding revision |
| Harness → Cognition/Plan | `[~]` | typed adapter、checkpoint/pin、stage summary | 对话不能打开认知阶段、调查、计划节点或 revision diff |
| Plan → AgentTemplate/Skill | `[~]` | plan/session/Skill pin 与 runner receipt | 缺 message/turn/plan-node/agent-invocation 映射 |
| Agent → Child/Collaborator | `[~]` | ChildTask、child_agent runner、session snapshot | UI 只有 runner 字样，无父子图、状态、进度、阻塞和输出导航 |
| Agent/Plan → Tool/LTW | `[~]` | lifecycle event、durable replay、tool_call_id | 工具未稳定关联 message、plan node 和 child agent |
| Turn → Memory View | `[~]` | snapshot/view digest、profile、selection reason | 点击用户输入不能查看本轮实际五层 view；无安全内容端点 |
| Turn/Agent → Approval | `[~]` | ApprovalStore、HITL action、durable resume | 审批不是对话问题实体；不能 Approval ↔ Agent 问题双向返回 |
| Plan/Artifact → Assurance | `[~]` | 五层 gate、finding/evidence、closure | finding 不能反向定位 plan/agent/tool/artifact/message |
| Conversation ↔ Operations | `[~]` | 同一 Web UI、snapshot + SSE | 只有 tab 切换，无类型化 selection/deep-link/history |
| Event/Replay → UI | `[~]` | Conversation、Tool、Operations 各自 replay | 缺统一相关性投影、跨 Store join 和 orphan 检测 |

## 3. 强制设计原则

1. **Conversation-first**：Conversation 是用户交互索引，不是另一个孤立 Store。
2. **Typed correlation**：所有跳转由类型化引用驱动，禁止字符串拼接猜测。
3. **Canonical source**：UI projection 只来自 durable Store/event；前端只保存 selection 和布局。
4. **Revision-aware**：导航固定 object revision/digest；新 revision 到达时显式提示，不静默换对象。
5. **Bidirectional**：Conversation → Inspector 与 Inspector → source message 必须同时成立。
6. **Fail closed**：关联缺失显示 `orphaned/unavailable`，不得错误挂接到“当前”任务。
7. **Display safety**：私有 chain-of-thought、secret、raw credential、未授权 Memory 正文永不进入 UI contract。
8. **No authority uplift**：候选答案、Agent 自报和 UI 状态不能成为完成、审批或验收权威。
9. **Replay stable**：断线、刷新、重启后相同 deep-link 必须解析到相同 revision，或明确报告已压缩/失效。
10. **Cross-client parity**：Web/TUI/ImGui/CLI 消费同一 projection；交互形态可不同，语义和状态不得漂移。

## 4. 统一身份与 Interaction Graph 契约

### 4.1 `InteractionRef`

建议新增 `agent.ui.interaction_ref/v1`：

```cpp
struct InteractionRef {
  std::string tenant_id;
  std::string conversation_id, turn_id, message_id;
  std::string task_id, run_id, harness_id;
  std::string plan_id, plan_node_id;
  std::uint64_t plan_revision{0};
  std::string agent_template_id, agent_invocation_id, child_agent_id;
  std::string skill_node_id, tool_invocation_id;
  std::string memory_snapshot_id, memory_view_digest;
  std::string approval_id, evidence_id, finding_id, artifact_id;
  std::string object_revision_digest;
};
```

字段可空，但每种对象类型必须定义最小身份集合。例如 child agent 至少绑定 tenant/conversation/turn/run、
agent invocation 和 child id；approval 至少绑定 tenant/turn/run、approval id、发起 message 和恢复节点。

### 4.2 `InteractionNode` / `InteractionEdge`

节点类型固定为：`message`、`thinking`、`cognition_stage`、`plan`、`plan_node`、`memory_view`、`agent`、
`skill_node`、`tool_invocation`、`approval`、`evidence`、`finding`、`artifact`、`closure`。

边类型至少包括：

- `originated_from`：过程/审批/产物来自哪条消息；
- `planned_by` / `implements`：Agent/工具实现哪个 plan node；
- `delegated_to` / `parent_of`：父子与协同关系；
- `used_memory_view`：LLM/Agent 实际使用的 Memory View；
- `requested_approval` / `resumes`：问题、审批与恢复节点；
- `produced` / `verified_by` / `supports` / `contradicts`：artifact/evidence/finding；
- `supersedes`：plan、memory、approval、artifact 的 revision 演进；
- `closed_by`：criterion/finding 与 closure decision。

每条边带 source store、source revision、digest、created_at 和 visibility。

### 4.3 `UiInteractionEvent`

统一 display-safe 事件至少包含：

```json
{
  "schema_version": "agent.ui.interaction_event/v1",
  "event_id": "...",
  "sequence": 1,
  "event_type": "agent.status.changed",
  "visibility": "user|operations|audit",
  "primary_ref": {},
  "related_refs": [],
  "display": {"label": "", "summary": "", "status": "running"},
  "navigation_target": {"view": "agents", "object_id": "..."},
  "source_revision": {},
  "timestamp": "...",
  "digest": "sha256:..."
}
```

### 4.4 `InteractionProjectionStore`

新增 revision-aware、digest-verified 的 durable projection store：

- 幂等 ingest、CAS snapshot、event sequence、retention floor、archive boundary；
- 按 turn/message/plan node/agent/approval/evidence 查询；
- 保存关联图，不复制 secret 或私有 reasoning；
- 记录 orphan、ambiguous、redacted、compacted 状态；
- 跨 Store join 必须保存每个 source revision/digest，不实现虚假的跨库 ACID；
- source drift 时保持上一份一致 snapshot 并进入 stale/manual-review。

## 5. 对话驱动的 UI 行为

### 5.1 对话语义锚点

每个 turn 根据真实关联动态显示 chips：

`Thinking`、`Plan rN`、`Memory 5 layers`、`Agents N`、`Tools N`、`Approval required`、
`Evidence N`、`Artifacts N`、`Verified/Blocked`。

chip 不以计数模拟完成；没有 canonical ref 时不显示或显示 unavailable。流式过程中状态可从
`thinking → planning → delegated → executing → awaiting approval → verifying → completed/blocked` 演进。

### 5.2 Think & Plan

点击 Thinking 或 Plan：

- 打开 Planner Inspector 的 Think & Plan；
- 定位当前 turn 的 cognition pipeline 和 plan revision；
- 展示需求理解、事实、假设、未知项、调查来源、边界、任务 DAG、责任 Agent、产物和验收标准；
- 展示 critic/replan 结果和 revision diff；
- 只显示 `displayable_reasoning`、`reasoning_summary` 和结构化工作流结果。

### 5.3 Five-layer Memory

点击用户输入或 Memory chip，显示该 turn **实际绑定**的五层 view：

1. System；2. Organization；3. Project；4. Task；5. Turn/Runtime。

每层显示 source、authority、freshness、revision/digest、selected/excluded、selection reason、conflict、
token budget、进入哪些 LLM invocation。默认摘要；正文必须经过 ACL/visibility/redaction endpoint。
RAG、Skill、当前提示、历史尝试作为 Turn/Runtime 的 typed segment 显示，不伪装成长期记忆。

### 5.4 Agents / Collaboration

点击 `Agent N` 或 `Child Agent 2`：

- 定位父/子/协同 Agent 图；
- 显示角色、任务、plan node、AgentTemplate/Skill pin、runner、状态、进度、预算、token、latency；
- 显示当前/历史工具、checkpoint、输出 digest、evidence/artifact、blocking reason；
- 支持返回来源消息和关联计划节点；
- cancel/retry/request-input 等动作必须走现有 policy/runtime，UI 不直接改状态。

### 5.5 Approval ↔ Agent 问题

需要审批时，系统同时创建：

- 对话内 `approval-question` 消息卡；
- Approval Inspector 中的 accountable request；
- Operations HITL projection；
- Harness interruption/resume binding。

卡片展示 Agent 原问题、审批原因、影响范围、风险、计划/参数/memory digest、候选动作、身份、多签进度、
截止时间和恢复节点。`返回 Agent 问题` 必须跳转并高亮 source message；从问题卡点击审批则定位同一
approval revision。approve/reject/remediate/escalate/edit/delegate 只能调用真实 ApprovalStore/PDP/action service。

### 5.6 Assurance 与证据

点击五层 Assurance 或 finding，必须可定位 criterion、plan node、Agent、tool receipt、artifact 和 source
message；修复后能看到旧证据失效、新 artifact、reverification 和 closure decision。任何“passed”都必须带
可解析 evidence ref。

### 5.7 Deep-link 与选择状态

建议路由：

```text
#turn/{turn_id}
#turn/{turn_id}/thinking
#turn/{turn_id}/memory
#plan/{plan_id}/revision/{revision}/node/{node_id}
#agent/{agent_invocation_id}
#approval/{approval_id}
#evidence/{evidence_id}
#artifact/{artifact_id}
```

浏览器 history、刷新恢复、键盘导航和焦点管理必须工作。新事件不能抢走用户当前 selection；目标 revision
过期时显示 superseded banner，并允许跳转最新 revision。

## 6. 连续实施批次

### P4-UIX0 — 基线、需求与关联审计 `[x] local`

交付：模块接线清单、production/demo/callback 边界、身份字段映射、orphan inventory、UI journey、风险清单。
退出门槛：每个当前 UI 对象能追溯 canonical source；无法追溯者显式列为缺口，不能以当前选中 run 补齐。

### P4-UIX1 — Interaction contracts `[x] local`

交付：InteractionRef/Node/Edge/Event/Snapshot、版本化 JSON 编解码、validation、digest、visibility。
退出门槛：非法组合、跨 tenant、缺最小身份、unknown field、digest 篡改、私有 reasoning 均 fail-closed。

### P4-UIX2 — Durable projection 与 source adapters `[~]`

交付：SQLite InteractionProjectionStore；Conversation、Harness、Planning、Memory、AgentTemplate/ChildTask、
Tool/LTW、Approval、Assurance/Artifact/Closure adapters；replay/compaction/orphan detector。
退出门槛：同一事件重复 ingest 不重复节点；重启后 graph/deep-link 一致；source revision drift 不错误拼接。

### P4-UIX3 — Conversation semantic anchors `[x] local`

交付：turn/message view model、chips、流式状态、source highlight、Conversation ↔ Inspector selection controller。
退出门槛：多个并发 turn、刷新、SSE replay 后 chip 数量、状态和 target ref 一致。

### P4-UIX4 — Think & Plan Inspector `[x] local`

交付：认知阶段、调查证据、结构化 reasoning summary、计划 DAG、revision diff、plan node details。
退出门槛：点击对话 Thinking 精确定位其 turn/plan；不得显示 chain-of-thought；replan 可查看父 revision。

### P4-UIX5 — Five-layer Memory Inspector `[x] local`

交付：五层分组、selection/exclusion、authority/freshness/conflict/token、invocation usage、安全摘要/正文端点。
退出门槛：点击用户输入只显示该 turn pinned view；跨 tenant/未授权正文拒绝；snapshot drift 明示。

### P4-UIX6 — Agent collaboration Inspector `[x] local`

交付：父子/协同图、Agent/Skill/runner/plan node 状态、tool timeline、budget/progress/blocker、source navigation。
退出门槛：点击 Child Agent 2 定位唯一 invocation；父子 grant、状态、checkpoint 和 terminal receipt 一致。

### P4-UIX7 — Approval conversation loop `[~]`

交付：approval-question message、Approval Inspector、双向导航、identity/SoD/multisig/delegation/edit/escalation、
decision 后 durable resume 状态。
退出门槛：pending/approved/rejected/expired/revoked/stale 全路径；返回问题定位正确；无 IdP 时 fail-closed。

### P4-UIX8 — Assurance/Evidence/Artifact navigation 与跨端适配 `[x] local`

交付：finding/evidence/criterion/artifact 反向链接；Web/TUI/ImGui/CLI 同源 selection semantics；操作审计。
退出门槛：finding → remediation → new artifact → reverification → closure 可完整导航；跨端状态一致。

### P4-UIX9 — 综合认证、真实 UI 与迁移关闭 `[~]`

交付：golden interaction task、故障/重启/replay/并发矩阵、性能与可访问性、真实 Web/TUI/ImGui 截图、旧
projection compatibility 和 migration report。
退出门槛：第 8 节所有 DoD 关闭；状态/追溯/执行台账更新；残余部署项明确，不以 screenshot 代替数据证据。

## 7. 实施依赖与顺序

强制顺序：

```text
UIX0 → UIX1 → UIX2 → UIX3
                    ├→ UIX4
                    ├→ UIX5
                    ├→ UIX6
                    └→ UIX7
UIX4–UIX7 → UIX8 → UIX9
```

UIX4–UIX7 可在 UIX3 之后分支实现，但共享同一 selection controller 和 Interaction contract，禁止各自定义
私有 ID/事件。UIX8 前必须完成 Approval 与 Memory visibility 安全审计。

## 8. 验证矩阵与总 DoD

### 8.1 测试层次

| 层次 | 强制场景 |
|---|---|
| Contract | schema、unknown field、minimal identity、digest、visibility、cross-tenant |
| Module | graph ingest/query、selection reducer、router、revision diff、redaction |
| Integration | Conversation→Plan、Turn→Memory、Plan→Agent→Tool、Approval→resume、Finding→closure |
| Recovery | process kill、SQLite reopen、SSE replay、compaction boundary、stale source revision |
| Concurrency | 两 turn、两 child agent、approval revision race、plan replan、late event |
| Security | IDOR、tenant escape、hidden reasoning、Memory ACL、credential/prompt leakage、forged refs |
| UI | mouse/keyboard/history/refresh/responsive/empty/error/loading/superseded/orphan states |
| System | 一个真实任务贯穿 cognition→plan→memory→agents/tools→approval→assurance→closure |
| Metrics | navigation resolve rate、orphan rate、projection lag、replay recovery、错误关联率、a11y |

### 8.2 Golden interaction task

固定任务必须产生：一个用户 turn、displayable thinking、至少两级计划、五层 Memory View、一个父 Agent、两个
子/协同 Agent、至少两个工具、一个 artifact、一个 pending approval、一次重启恢复、一个 finding、一次修复与
复验以及 verified closure。自动化逐项点击并验证 URL、selection、source refs、revision 和显示内容。

### 8.3 Mandatory DoD

- [ ] 所有核心对象有 canonical InteractionRef 和 source revision/digest；
- [ ] Conversation 是双向导航主索引；
- [ ] Thinking 点击显示同 turn 的认知/计划过程；
- [ ] 用户输入点击显示同 turn 实际 pinned 五层 Memory View；
- [ ] Child/Collaborative Agent 点击显示真实状态、工作、工具、产物和阻塞；
- [ ] Approval 与 Agent 原问题双向跳转，并接真实 PDP/identity/resume；
- [ ] Assurance/finding/evidence/artifact 可追溯到计划、Agent、工具和消息；
- [ ] 刷新、断线 replay 和进程重启后 deep-link 仍解析或显式报告失效；
- [ ] hidden reasoning、secret、未授权 Memory 正文泄漏为 0；
- [ ] orphan/ambiguous/cross-tenant 关联 fail-closed；
- [ ] Web/TUI/ImGui/CLI 使用同源 projection；
- [ ] 真实 UI 截图覆盖 Think & Plan、Memory、Agents、Approval、返回问题、finding 导航；
- [ ] Golden interaction task 全链路通过并由 TaskClosureController 验证；
- [ ] 状态矩阵、traceability、execution ledger 和 residual 更新。

任一项未关闭，Interaction UI 和“综合完整 Agent 框架”保持 `[~]`。

## 9. 计划交付文件位置

候选代码位置：

- `include/agent/ui/interaction_graph.hpp`
- `src/ui/interaction_graph.cpp`
- `include/agent/ui/interaction_projection_store.hpp`
- `src/ui/sqlite_interaction_projection_store.cpp`
- `include/agent/ui/interaction_source_adapters.hpp`
- `src/ui/interaction_source_adapters.cpp`
- `include/agent/ui/interaction_selection.hpp`
- `src/ui/interaction_selection.cpp`
- `examples/web_ui_static/`、TUI/ImGui presentation adapters
- `tests/test_interaction_*.cpp`、`tests/scripts/test_interaction_ui_*`

文档和证据：

- 本计划；
- `phase-4-status.md` 的 Explainable UI/Operations 与综合接线状态；
- `phase-4/traceability-matrix.md`；
- `phase-4/execution-ledger.md`；
- 真实截图和 UI 自动化报告。

## 10. 风险与决策

| 风险 | 控制决策 |
|---|---|
| 复制所有 Store 形成第二事实源 | projection 只保存 display-safe refs、source revision/digest |
| chain-of-thought 泄漏 | 契约只接受显式 displayable summary；adapter redaction + negative tests |
| Memory UI 造成跨 scope 泄漏 | ACL 在服务端执行；默认摘要；tenant/scope/authority 校验 |
| late/out-of-order event 错挂接 | sequence + revision + digest + orphan queue；禁止 current-run fallback |
| deep-link 指向已压缩事件 | retention boundary/archive ref；显示 compacted，不静默替换 |
| UI 动作绕过控制面 | 所有 action 进入既有 PDP/Approval/Runtime service |
| 四端各自实现导致漂移 | 共享 projection/selection reducer；端侧只负责呈现 |
| 为满足 demo 注入假 Agent/Memory | production composition 禁止 fixture；demo 明确标记 synthetic |

## 11. 实施授权边界

用户已于 2026-08-15 明确批准 `P4-UIX0 → P4-UIX9` 连续实施。UIX3–UIX9 的 UI 变更已在真实 Web runtime
和 Chrome 中进行截图验收；这些本地证据不替代真实 IdP、多签、production Store、跨主机或 external Live 证据。

## 12. 2026-08-15 本地实施结果

- 新增 `agent.ui.interaction_{node,edge,event,snapshot}/v1`、严格 JSON/digest/visibility/minimal identity 和
  hidden-reasoning/secret fail-closed 校验。
- 新增 SQLite WAL/FULL `InteractionProjectionStore`：stream CAS、幂等 event、节点/边 revision、visibility
  query、replay、restart 和 orphan edge 自动恢复。
- 新增 Operations→Interaction source adapter，将 Message、Thinking、Plan/PlanNode、MemoryView、Agent/Skill、
  Approval、Evidence/Finding/Artifact 关联到 source revision/digest；不复制 raw prompt、Memory 正文或 credential。
- Web 新增 `/ui/interactions/snapshot`，Conversation 与 Think & Plan、Memory、Agents、Approval、Assurance Inspector
  双向选择、fragment deep-link、来源显示、返回原问题和 related-object 导航。
- `UIManager::publish_interactions` 将同一 canonical snapshot 广播给 Web/TUI/ImGui/CLI；CLI 输出同源摘要，
  TUI/ImGui 继续经统一 aux event 消费，不自建事实模型。
- 本地门禁：`interaction_graph`、`interaction_projection_store`、`interaction_source_adapters`、
  `phase4_operations_ui` 4/4 PASS；`test_web_ui_static.sh` PASS；真实 Chrome 截图覆盖 Think & Plan、Memory、
  Agents、Approval、Assurance。

## 13. UIXR0–UIXR8 Residual Closure 结果

- `ProductionInteractionAssembler` 已直接读取 Conversation、Run、Plan、Session/ChildTask、Approval、Assurance 和
  Remediation Store；required store/object、tenant/task/run identity、positive revision、重复节点与 unsafe display
  均 fail-closed。Operations adapter 仅保留为兼容/演示来源，不再冒充生产 adapter。
- `SessionStore::load_current()` 提供无创建副作用的当前快照读取；真实两级 ChildTask 与 ToolCommit 被投影为
  `parent_of`、`delegated_to`、`produced` 关系。
- Approval action service 已覆盖 authenticated decision、edit→新 request、delegation 和 escalation；Web 在未配置
  authenticated session 时继续 fail-closed。真实 OIDC/JWT session rotation 与 KMS 签名仍属于外部认证残项。
- Remediation checkpoint 可投影 finding、修复 action、stage artifact、reverification plan 与 ready-for-execution
  closure，保留 invalidated/reusable evidence 导航事实。
- Web 已以 SQLite projection 为读取源，提供 snapshot、event replay 与 object deep-link 端点；每次 projection
  revision 独立递增，source revision/digest 保持不变，避免观察更新与领域 revision 混淆。
- TUI/ImGui presentation model 严格解码同一 InteractionSnapshot；TUI Operations 面板显示对象图与选中摘要，
  ImGui 新增 Interaction Graph 表格页，CLI/Web 继续消费同一契约。
- 本地证据：8 项 targeted CTest 全通过；Web static PASS；Web/TUI/ImGui build PASS；真实 Web runtime/Chrome
  1600×1000 截图 `/tmp/p4-uixr-web.png` 已检查布局、可读性与 degraded 状态呈现。

仍开放且不得误报完成：真实 OIDC/JWT/KMS 多签、跨主机 projection replay/HA、生产数据驱动的完整
finding→remediation→reverification→verified closure Live campaign。故本地 residual closure 已完成，Phase 4
外部生产认证总状态仍为 `[~]`。
