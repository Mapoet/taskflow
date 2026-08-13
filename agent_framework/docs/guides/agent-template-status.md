# AgentTemplate 综合运行时状态

更新日期：2026-08-13

## 结论

AT0–AT8 的目标主链已经形成正式源码实现：

`AgentTemplateRegistry → CandidateRetriever → SkillPlanProvider → PlanValidator → ActiveSkillSession → SkillWorkflowCompiler → SkillRunner → CompletionAuthority → Operations`

这条链不是 `AgentLoopNode` 的 prompt 注入别名。`AgentRuntime::run` 是统一入口；默认要求 completion authority，Runner 成功或模型 final 均不能直接形成 verified completion。

## 能力状态

| 能力 | 状态 | 生产口径 |
|---|---:|---|
| 版本化 AgentTemplate contract | `[x]` | canonical digest、future schema fail-closed |
| SQLite Template/Invocation registry | `[x]` | immutable revision、CAS、WAL、restart recovery |
| Candidate retrieval/session pin | `[x]` | deterministic Top-K、package/dependency/capability/generation pin |
| Fixed/Directive/Model/Hybrid planning | `[x]` | 类型化 provider SPI；模型输出必须经过确定性 validator |
| 通用 plan validator | `[x]` | DAG、required、effect/approval、permission、budget、revision/effect continuity |
| Runner SPI/compiler | `[x]` | 8 类 runner、统一 event/receipt、bounded DAG concurrency |
| AgentRuntime/Workflow adapters | `[x]` | Standalone、WorkflowNode、WorkflowSubflow；Conversation/RemoteA2A 由 hosting contract 表达 |
| Replan/Closure | `[x]` | 9 类 trigger、revision CAS、TaskClosure adapter、fail-closed authority |
| Operations/UI | `[x]` | canonical projection；CLI/Web/TUI/GUI 共享；Web 实机截图验证 |
| 完整生产认证 | `[~]` | 离线综合链已认证；真实模型、MCP、ChildAgent、RemoteA2A 和 crash/restart soak 仍需环境认证 |

## 剩余限制

- `CallbackSkillRunner` 是 SPI 测试/嵌入适配器，不等同于各后端的生产 connector；各生产 runner 仍须绑定既有 ToolRuntime/MCP/A2A/Approval durable backend。
- 当前 compiler 提供单次 DAG 执行和统一 receipt，但跨进程 checkpoint/attach/reconcile 需由具体 durable runner 完成。
- Conversation 与 RemoteA2A 已具 hosting identity；默认产品 composition 尚需逐入口迁移到 `AgentRuntime`。
- UI contract 已完整暴露目标字段；TUI/GUI 共享 projection 的契约回归已通过，Web 已完成实机视觉检查。

因此当前技术判断是：目标架构主链已实现并可集成，生产后端全量绑定与真实环境认证尚未完全闭环，不能宣称整个系统 production-certified。
