完成基于 agent_framework/docs/af-target.md 的现状审计。结论明确：

  > 当前 AF 已具备较完整的 Skill、Tool Runtime、Harness、Conversation、Workflow、Approval 和 Closure 底座，但尚未集成为 AgentTemplate →
  > SkillCollaborationPlan → ActiveSkillSession → Compiler → Runner → Assurance/Closure 的统一综合系统。

  代表性基础回归 16/16 通过，证明底层组件可复用；但无法证明目标架构已实现，因为十个目标核心组件目前仅存在于设计文档，没有对应正式源码契约。

  ## 一、当前能力满足度

   目标能力                               当前状态    判断
  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   Skill Registry/Loader/资源/供应链        85–90%    底座较成熟
  ─────────────────────────────────────  ──────────  ─────────────────────────────────────────────────────────────────────────────
   Skill 权限交集与 Capability Binding      75–85%    单 Skill 路径可用
  ─────────────────────────────────────  ──────────  ─────────────────────────────────────────────────────────────────────────────
   Skill Workflow DAG                       65–75%    支持工具、嵌套 Workflow、ChildTask、checkpoint，但不是通用协作计划编译器
  ─────────────────────────────────────  ──────────  ─────────────────────────────────────────────────────────────────────────────
   Durable Tool Runner                      75–85%    生命周期较完整，但没有统一 SkillRunner SPI
  ─────────────────────────────────────  ──────────  ─────────────────────────────────────────────────────────────────────────────
   Model-driven Skill Planning              10–20%    只有关键词匹配和 prompt injection，不是类型化协作规划
  ─────────────────────────────────────  ──────────  ─────────────────────────────────────────────────────────────────────────────
   Directive-driven Agent Template           5–10%    缺正式 Template contract、registry 和受控开放槽
  ─────────────────────────────────────  ──────────  ─────────────────────────────────────────────────────────────────────────────
   Workflow-embedded Agent Template         15–25%    有通用 SubflowNode，但没有 AgentTemplateNode
  ─────────────────────────────────────  ──────────  ─────────────────────────────────────────────────────────────────────────────
   Active Skill Session/完整 pinning        20–30%    局部 dependency lock，缺统一 invocation manifest
  ─────────────────────────────────────  ──────────  ─────────────────────────────────────────────────────────────────────────────
   动态重规划                               10–20%    没有类型化触发、revision CAS 和权限/预算不扩张验证
  ─────────────────────────────────────  ──────────  ─────────────────────────────────────────────────────────────────────────────
   统一 Runner/事件/receipt                 35–45%    能力散落于多个模块，语义未统一
  ─────────────────────────────────────  ──────────  ─────────────────────────────────────────────────────────────────────────────
   Assurance/Closure 接入                   55–65%    Harness 很强，但尚未成为所有 AgentTemplate 执行的强制出口
  ─────────────────────────────────────  ──────────  ─────────────────────────────────────────────────────────────────────────────
   UI/Operations                            30–40%    现有运行视图无法完整呈现协作计划、Skill DAG、session pins 和 runner receipt
  ─────────────────────────────────────  ──────────  ─────────────────────────────────────────────────────────────────────────────
   生产认证                                 20–30%    底层专项认证较多，但目标综合链没有集成认证证据

  ## 二、核心断点

  当前真正缺失的是以下正式生产组件：

  1. AgentTemplate contract 与版本化 registry。
  2. SkillCollaborationPlan 正式类型。
  3. ActiveSkillSession 及不可变 pinning。
  4. SkillCandidateRetriever。
  5. SkillPlanProvider：Model、Directive、Hybrid、Fixed。
  6. SkillCollaborationPlanValidator。
  7. SkillWorkflowCompiler。
  8. SkillRunner SPI、registry 和统一生命周期。
  9. AgentRuntime 统一入口。
  10. AgentTemplateNode/Subflow Workflow adapter。

  现有 SkillWorkflowRuntime 不应废弃，但只能作为新 Compiler/Runner 内核的一个执行后端。现有 AgentLoopNode 的：

  Registry match → active_skill_id → prompt injection

  仍是主要架构债务。

  ## 三、连续实施计划（2026-08-13 状态）

  当前进度：AT0–AT8 已实现并通过专项测试；AT9 离线认证已完成。真实外部 provider、MCP/A2A 和长时间 crash/restart soak 属于环境认证残项，详见 `docs/guides/agent-template-status.md`。

  ### AT0：目标基线与契约冻结

  落地：

  - agent/template/types.hpp
  - AgentBusinessMode
  - AgentHostingMode
  - AgentTemplate
  - AgentTemplateInvocation
  - SkillCollaborationPlan
  - SkillPlanNode/Edge
  - ActiveSkillSession
  - typed artifact/evidence/receipt references
  - JSON 序列化、schema version、canonical digest

  门禁：

  - schema round-trip
  - unknown-version fail-closed
  - canonical digest 稳定性
  - business mode 与 hosting mode 正交测试

  ### AT1：Template Registry 与 Invocation Pinning

  实现：

  - SQLite-backed AgentTemplateRegistry
  - revision CAS
  - immutable template snapshot
  - Skill、Model、Prompt、Permission、Budget、Capability、Deployment generation pinning
  - invocation manifest digest chain

  门禁：

  - restart recovery
  - concurrent revision conflict
  - dependency drift
  - future schema rejection
  - manifest tamper detection

  ### AT2：Candidate Retriever 与 Active Skill Session

  实现：

  - capability/keyword/role/policy 多维检索
  - deterministic Top-K
  - dependency resolution
  - Skill package revision pin
  - per-Skill grant intersection
  - capability binding snapshot
  - session lease/state/checkpoint

  门禁：

  - 权限不能扩张
  - dependency mismatch fail-closed
  - registry reload 不影响活动 session
  - candidate ordering reproducible

  ### AT3：Plan Provider 与确定性 Validator

  实现四种 provider：

  - ModelSkillPlanProvider
  - DirectiveSkillPlanProvider
  - HybridSkillPlanProvider
  - FixedWorkflowPlanProvider

  Validator 强制检查：

  - DAG 无环
  - required node 不可删除
  - selector 必须可解析
  - effect 与 permission 相符
  - Write/Unknown 必须审批
  - budget/deadline 不超限
  - verifier/completion contract 完整
  - revision CAS
  - replan 不得否认已提交 effect

  ### AT4：统一 SkillRunner SPI

  统一生命周期：

  admit → prepare → start/attach → observe
  → checkpoint → cancel → reconcile → finalize receipt

  接入：

  - InlinePrompt
  - LocalCapability
  - SandboxedProcess/CLI
  - MCP
  - ChildAgent
  - NestedWorkflow
  - HumanApproval

  所有 Runner 输出统一：

  - typed events
  - artifact refs
  - evidence refs
  - checkpoint refs
  - effect receipts
  - terminal receipt

  ### AT5：SkillWorkflowCompiler

  把 SkillCollaborationPlan 编译为 Taskflow DAG，支持：

  - Sequential
  - Parallel Map
  - Map-Reduce
  - Supervisor-Worker
  - Debate/Judge
  - Conditional Branch
  - Iterative Refinement
  - Verifier Pipeline
  - HITL
  - Nested Agent
  - Remote A2A

  必须实现：

  - typed input/output mapping
  - cancellation/deadline propagation
  - bounded concurrency
  - deterministic node identity
  - checkpoint/restart
  - partial result/ObjectStore
  - event projection

  ### AT6：统一 AgentRuntime

  提供目标 API：

  AgentTemplateRegistry registry;
  AgentRuntime runtime;
  runtime.run(template_ref, input, options);

  统一三条业务入口：

  - ModelDriven
  - DirectiveDriven
  - Hybrid

  统一宿主：

  - Standalone
  - Conversation
  - WorkflowNode
  - WorkflowSubflow
  - RemoteA2A

  现有单 Skill AgentLoop 迁移为单节点 CollaborationPlan，避免双内核长期并存。

  ### AT7：Assurance、Closure 与动态重规划

  实现类型化 replan trigger：

  - SkillUnavailable
  - PermissionDenied
  - ApprovalRejected
  - ToolFailed
  - EvidenceInsufficient
  - VerificationFailed
  - BudgetThresholdReached
  - UserChangedRequirement
  - StagnationDetected

  强制：

  - 新 revision 重新验证
  - 已提交 effect 不可回滚叙事
  - required node 不可删除
  - 权限和预算不可扩张
  - model-final 不能直接形成 verified completion
  - 最终状态必须经过 Harness、Assurance 和 TaskClosure

  ### AT8：UI 与 Operations 集成

  四端统一暴露：

  - Template/revision
  - business/hosting mode
  - collaboration plan DAG
  - node/runner lifecycle
  - ActiveSkillSession pins
  - permissions/approval
  - budget/usage
  - artifacts/evidence
  - replan history
  - verifier/closure status
  - cancel/resume/retry/replay actions

  涉及 UI 时将按要求启动真实环境并提供真实截图验证。

  ### AT9：系统性测试与认证

  测试分层：

  1. Contract：schema、digest、revision、manifest。
  2. Module：Retriever、Validator、Session、Runner、Compiler。
  3. Integration：Template→Plan→DAG→Runner→Assurance→Closure。
  4. Hosting parity：Conversation、WorkflowNode、Subflow、A2A 输出语义一致。
  5. Recovery：进程退出、MCP 断连、checkpoint 恢复、stale owner fencing。
  6. Security：权限泄漏、凭据传播、TOCTOU、effect replay、审批绕过。
  7. Performance：100+ Skill 检索、宽 DAG、深 DAG、长时间运行和 event replay。
  8. Live：真实 LLM、MCP、Sandbox、A2A、Approval/IdP。
  9. UI：四端真实运行截图与操作闭环。
  10. Completeness：目标条款—源码—测试—证据双向追踪矩阵。

  ## 四、落盘文档

  实施时同步创建或更新：

  - docs/guides/agent-template-status.md
  - docs/guides/agent-template-implementation-plan.md
  - docs/guides/agent-template-traceability.md
  - docs/guides/agent-template-integration-test-matrix.md
  - docs/af-target.md 实现状态附录
  - docs/agent-framework2claude-code.md 对应技术状态

  每批只有在代码、测试、追踪矩阵和状态文档同时通过后，才进入下一批。

  ## 五、当前证据

  已运行底层代表性回归：

  16/16 passed

  覆盖：

  - Skill Registry/Manifest/Policy
  - AgentLoop Skill policy
  - Capability Runtime
  - Skill Workflow
  - Skill Lifecycle
  - Skill stress/scale
  - ChildTask/Subflow
  - Tool Runtime/LongTask
  - Harness-supported runtime
  - Task Closure

  这证明迁移底座稳定，但不构成目标系统完成证据。

  请确认批准按 AT0 → AT9 连续实施，测试通过后自动进入下一批。
