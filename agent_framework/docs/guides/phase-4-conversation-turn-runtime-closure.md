# P4-CTR0–CTR10：Conversation/Turn Runtime Closure

**基线日期**：2026-08-12  
**授权**：用户批准连续实施  
**状态**：本地工程闭环 accepted；外部 Production Live 保持独立开放

## 目标

在 GPC/GPW 的确定性 Task Closure 之上建立轻量、类型化、事件驱动且可恢复的 Conversation/Turn 层。Turn 只产生 candidate answer、工具 receipt、usage 和 stop reason，永远不能签发 completed_verified。

## 工作包与结果

| ID | 结果 | 验收 |
|---|---|---|
| CTR0 | 校准 full-project、GPW 和源码事实，冻结双循环边界 | 不重复实现 ClosureController |
| CTR1 | Conversation、Turn、Profile、Outcome、Event、Context、Boundary v1 typed contract | 未知 profile fail-closed；Turn verified 拒绝 |
| CTR2 | SQLite WAL/FULL append-only ConversationStore | message parent/digest chain、tenant isolation、Turn CAS、restart |
| CTR3 | TurnStateMachine | 非法/terminal transition 拒绝；interrupt/resume 有界 |
| CTR4 | RuntimeEventEnvelope | durable/ephemeral、visibility、sequence、digest |
| CTR5 | ConversationEngine | start/continue/interrupt/resume/input routing/event sink |
| CTR6 | ContextProjection/CompactBoundary | mandatory contract/policy/citation 禁止静默裁剪 |
| CTR7 | 六档 TaskExecutionProfile 与单向升级 | side effect 在 production 低 profile 下拒绝 |
| CTR8 | AcceptanceContract→ClosureContract 与 GraphExecutor→TurnOutcome bridge | 后者强制清除 verified 权威 |
| CTR9 | 五 LiveRuntime 入口共享 AGENT_TASK_PROFILE bootstrap | AgentServer/CLI/Web/TUI/ImGui 构建 |
| CTR10 | 七层回归 | Phase4 74/74；Phase3 22/22；100 Turn |

## 类型化边界

ConversationEngine → ModelTurnOutcome（unverified）→ execution receipts → ProductionTaskRuntime → Harness/Assurance/Remediation → TaskClosureController → completed_verified。

六档 profile 为 conversation、read_only_analysis、artifact_delivery、code_change、external_action、professional。未知值拒绝；发现副作用只能升级，不能静默降级。

## 验收证据

- phase4_conversation_runtime：100 Turn、200-message parent chain、restart、CAS、event sequence、compact mandatory protection、profile/contract/Graph bridge。
- 事件 sequence 改为 SQLite MAX(sequence)，100 Turn 实测由约 17.4 秒降到约 5.4–6.7 秒（Debug/WAL/FULL；不是生产性能认证）。
- phase4-offline：74/74 PASS；phase3：22/22 PASS；五 demo target 构建 PASS。
- 真实进程 negative：未知 AGENT_TASK_PROFILE 拒绝；production code_change 直接 React 拒绝。
- 本批没有前端布局/视觉变化，因此不生成无意义的新截图；既有真实 UI completion-state 截图和 UI contract regression 继续有效。

## 未关闭边界

CTR 建立了 Conversation control/data plane 与统一 bootstrap，但五个交互 demo 的实际模型执行仍经兼容 run_react_cli_sync adapter。下一阶段应实现统一 Tool Lifecycle Kernel 后，再完成实时 Turn 全切换。R6L 的真实 IdP/KMS/provider/scheduler/bundle 与真实多节点 chaos 均未在本批宣称完成。
