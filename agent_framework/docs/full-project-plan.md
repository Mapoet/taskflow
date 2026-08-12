 基于 agent_framework/docs/full-project.md 与当前 Phase 4/CSAC 实现交叉审计后，下一阶段不应继续扩展外围模块，
  而应关闭项目最核心的架构分裂：

  > 让所有生产任务只有一种完成语义：VERIFIED_COMPLETE，模型的 final_answer/is_final 只能结束一次 LLM 回合，不能
  > 证明任务完成。

  当前最短板已经从“缺少基础模块”转为“旧 AgentLoop 与新 Harness 双轨运行，生产完成权威尚未统一”。

  ## 一、审计结论

  ### 1. 当前存在两种不等价完成语义

  旧路径：

  AgentLoop
    → tool_calls.empty()
    → is_final 或 final_answer 非空
    → 循环退出

  该路径仍把以下情况写成 is_final=true：

  - LLM 调用异常；
  - 同轮重复工具调用；
  - 模型返回非空答案；
  - 取消、deadline 或循环退出。

  因此，is_final 实际混合表达：

  - 模型结束发言；
  - guard 中止；
  - 执行失败；
  - 预算/时间终止；
  - 任务完成。

  新 Harness 则要求 AcceptanceContract、Artifact、Evidence、Oracle、Approval、Reverification 和 Judge 等确定性
  门禁闭合。两者不是同一个语义层。

  ### 2. 已有能力足以构建统一闭环

  当前无需另造一套执行系统，可以直接组合：

  - Cognition/Planning workflow；
  - ProductionWorkflowInputRepository；
  - ApprovalStore/PDP；
  - Artifact executor/journal；
  - Professional Assurance 与 production Oracle；
  - Remediation/Reverification；
  - RunStore/HarnessStore；
  - CSAC coordination journal；
  - Operations projection；
  - LLM invocation observability。

  真正缺失的是顶层 TaskClosureController 和所有生产入口的强制路由。

  ### 3. CSAC 是必要基础，但尚非统一完成控制器

  刚完成的 CSAC 已提供：

  - WAL/FULL durable coordination journal；
  - revision/digest participant；
  - restart、幂等、ManualReview；
  - production builder mandatory gate。

  但它仍主要是状态 pin/confirm 层，不负责：

  - 解释 AgentLoop 为什么停止；
  - 判断任务属于完成、阻塞还是停滞；
  - 计算跨轮进展；
  - 决定是否允许修复、重规划或继续消耗预算；
  - 将所有入口强制汇入 Harness completion gate。

  ## 二、建议下一连续批次

  批次名称：

  > P4-GPC0 → P4-GPC10：Golden-Path Task Closure

  总体目标：

  Input
    → Task Classification
    → Acceptance Contract
    → Bounded Cognition/Plan
    → Approved Execution
    → Artifact/Evidence
    → Deterministic Closure Evaluation
        ├─ VERIFIED_COMPLETE
        ├─ MINIMAL_REMEDIATION
        ├─ NEEDS_USER_INPUT
        ├─ BLOCKED_EXTERNAL
        ├─ STAGNATED
        ├─ BUDGET_EXHAUSTED
        ├─ MANUAL_REVIEW
        └─ FAILED/CANCELLED

  ## 三、详细工作包

  ### P4-GPC0：冻结唯一完成语义

  定义统一终态：

  enum class TaskTerminalState {
      CompletedVerified,
      CompletedWithLimitations,
      NeedsUserInput,
      BlockedExternal,
      BudgetExhausted,
      Stagnated,
      FailedExecution,
      FailedVerification,
      ManualReview,
      Cancelled
  };

  每个终态必须包含：

  - reason_code
  - terminal_authority
  - acceptance_contract_digest
  - unsatisfied_criteria
  - artifact_refs
  - evidence_refs
  - finding_refs
  - last_progress_revision
  - budget_consumed
  - resume_token
  - recommended_next_action
  - limitations
  - receipt_digest

  明确约束：

  - AgentLoop::is_final 重命名或降级为 model_turn_complete；
  - final_answer 只是候选输出；
  - 只有 ClosureController 可以签发任务终态；
  - CompletedVerified 必须绑定 Harness terminal checkpoint 和 AcceptanceReport；
  - guard、异常、超时和预算耗尽不得映射为完成。

  ### P4-GPC1：TaskClosureContract

  在现有 AcceptanceContract 之上增加轻量任务闭合合同：

  - task class：chat、read-only analysis、artifact delivery、code change、external action；
  - deliverables；
  - mandatory/optional criteria；
  - verification methods；
  - allowed side effects；
  - clarification policy；
  - budget；
  - remediation policy；
  - completion policy；
  - required Harness profile。

  复杂任务继续使用完整 Professional Assurance；简单问答使用轻量 contract，但也必须区分“已回答”和“已验证完成”。

  合同生成采用：

  LLM contract proposal
    → deterministic schema/policy validator
    → optional HITL
    → immutable revision/digest

  禁止 LLM 自行删除 mandatory criterion。

  ### P4-GPC2：TaskClosureController

  新增唯一确定性完成判定器，输入：

  - TaskClosureContract；
  - Harness checkpoint；
  - Run checkpoint；
  - current artifact/input revisions；
  - AcceptanceReport；
  - Approval decisions；
  - findings；
  - progress ledger；
  - budget/deadline；
  - external dependency status。

  输出：

  - terminal state；
  - next transition；
  - mandatory diagnostic；
  - coordination operation；
  - operations projection。

  优先级建议：

  Cancelled
  > ManualReview/unknown side effect
  > NeedsUserInput
  > BlockedExternal
  > BudgetExhausted
  > FailedExecution
  > FailedVerification
  > Stagnated
  > MinimalRemediation
  > CompletedWithLimitations
  > CompletedVerified

  强反证永远优先于模型完成声明。

  ### P4-GPC3：Durable Progress Ledger

  每个阶段/轮次持久化：

  - acceptance coverage delta；
  - newly valid evidence；
  - new artifact digests；
  - resolved/new findings；
  - severity delta；
  - plan semantic digest；
  - tool observation digests；
  - side-effect receipts；
  - token/cost/time/tool-call consumption；
  - blocker变化；
  - information-gain explanation digest。

  计算确定性进展：

  positive =
      newly_closed_mandatory_criteria
    + newly_valid_strong_evidence
    + new_artifact_revisions
    + resolved_findings
    + reduced_finding_severity

  negative =
      new_blockers
    + repeated_effects
    + invalidated_evidence
    + regression_findings

  不得仅以 plan revision 增加、提示词变化或 LLM 自述作为进展。

  ### P4-GPC4：跨轮停滞检测与 bounded replan

  检测：

  - 相同 evidence digest；
  - 相同 artifact digest；
  - semantic-equivalent plan；
  - 相同 finding 集合；
  - 参数微调但 tool result digest 不变；
  - acceptance coverage 不增长；
  - cost 增长但 information gain 为零。

  策略：

  1. 第一次无进展：允许一次 targeted replan；
  2. 第二次无进展：分类原因；
  3. 缺事实 → NeedsUserInput；
  4. 外部依赖不可用 → BlockedExternal；
  5. 无安全动作 → ManualReview；
  6. 重复失败且达到上限 → Stagnated；
  7. 禁止继续无限 ReAct。

  ### P4-GPC5：AgentLoop 语义拆分

  修改 AgentLoop，使其只返回回合结果：

  ModelTurnCompleted
  ToolWorkRequested
  GuardStopped
  ProviderFailed
  DeadlineExceeded
  Cancelled
  ContextExhausted

  重点修正：

  - LLM 异常不再设置任务完成；
  - repeat guard 不再设置任务完成；
  - final answer 非空不再直接证明完成；
  - 达到 max iterations 映射为 bounded stop；
  - 每轮输出 artifact/evidence/progress candidates；
  - 保留兼容字段，但 production route 不消费旧完成布尔值。

  对 legacy/demo 路径可以继续显示回答，但必须标记：

  unverified_model_response

  ### P4-GPC6：Production Entry Router

  所有生产入口必须通过统一路由：

  - CLI；
  - Web；
  - TUI；
  - ImGui；
  - AgentServer；
  - A2A；
  - GraphExecutor；
  - Skill workflow；
  - ChildTask/Subflow。

  入口路由规则：

  - 显式 demo/test profile 才允许 legacy AgentLoop；
  - production profile 必须存在：
      - TaskClosureContract
      - DefaultProductionComposition
      - ClosureController
      - CSAC coordinator
      - durable stores
      - production dependency manifest

  - 缺任一依赖时 fail-closed；
  - A2A remote completion 必须重建本地 evidence，不接受远端自报 completed；
  - 子任务完成不能直接推出父任务完成。

  ### P4-GPC7：验证驱动的最小修复

  将现有 SIC/PAO remediation loop 提升为默认闭合策略：

  finding
    → impacted criteria
    → impacted artifact nodes
    → smallest authorized action
    → approval binding
    → execution receipt
    → new artifact revision
    → invalidated evidence set
    → selective production oracle rerun
    → closure reevaluation

  限制：

  - 不受影响的强证据只能在 revision/freshness/policy 均匹配时复用；
  - 修复 action 超出原批准范围时生成新 ApprovalRequest；
  - 连续相同 finding 不允许再次执行相同修复；
  - 修复轮数、成本、时间和风险均有上限；
  - 未知副作用进入 ManualReview。

  ### P4-GPC8：三个 Golden Tasks

  建立三个真正的纵向系统任务。

  #### Golden Task A：文件交付

  覆盖：

  - 创建指定文件；
  - 路径、内容、非空和 digest Oracle；
  - restart；
  - 重复投递；
  - 错误路径；
  - 用户修改要求；
  - 最终 CompletedVerified。

  #### Golden Task B：代码修复

  覆盖：

  - cognition/investigation；
  - plan/approval；
  - sandbox build/test；
  - 故意制造初次失败；
  - finding；
  - 最小修复；
  - 新 artifact digest；
  - 旧 evidence 失效；
  - selective reverification；
  - Judge/Operations；
  - crash/restart；
  - 最终验证完成。

  #### Golden Task C：外部依赖阻塞

  覆盖：

  - 外部 provider/凭据/服务缺失；
  - 调查证据；
  - 不执行危险替代操作；
  - 不 skip-as-pass；
  - 正确进入 BlockedExternal；
  - 保存 resume requirements；
  - 依赖恢复后可续跑。

  每个任务都测试：

  - success；
  - clarification；
  - cancellation；
  - timeout；
  - crash/restart；
  - duplicate delivery；
  - stale approval；
  - side-effect idempotency；
  - verification failure；
  - remediation upper bound；
  - terminal projection。

  ### P4-GPC9：Completion Authority 接入 Operations/Observability

  统一输出：

  - terminal state；
  - verified/unverified badge；
  - completion authority；
  - criteria coverage；
  - unresolved findings；
  - evidence strength；
  - artifact revision；
  - progress curve；
  - stop reason；
  - resume requirements；
  - budget/cost；
  - trace/coordination receipts。

  Observability 增加：

  - task.closure.state
  - task.closure.reason
  - task.progress.delta
  - task.criteria.closed
  - task.stagnation.count
  - task.remediation.cycles
  - task.false_completion_rejected
  - task.cost_per_closed_criterion

  本批若涉及 UI 展示变更，将按照项目指令使用真实运行环境截图验证 CLI/Web/TUI/ImGui 同源表现。

  ### P4-GPC10：系统性验收与文档校准

  测试层次：

  1. Contract：终态、合同、非法降级、未知 reason。
  2. Module：Closure evaluator、progress function、stagnation detector。
  3. Integration：AgentLoop→Harness、Approval、CSAC、Oracle、Remediation。
  4. System：三个 Golden Tasks。
  5. Security：伪造 completion、stale approval、跨 tenant evidence、远端自报完成。
  6. Recovery：每个 transition/commit/receipt crash point。
  7. Performance：进展账本规模、reconcile latency、token/tool 成本。
  8. UI：四端真实运行截图及交互。
  9. Regression：Phase 3/4、boundary、full build、diff check。

  ## 四、强制验收指标

   指标                                        门槛
  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ━━━━━━
   False verified completion                      0
  ──────────────────────────────────────────  ──────
   Guard/exception 映射为完成                     0
  ──────────────────────────────────────────  ──────
   Stale artifact/evidence 被接受                 0
  ──────────────────────────────────────────  ──────
   Stale/expired approval 被消费                  0
  ──────────────────────────────────────────  ──────
   已确认副作用重复执行                           0
  ──────────────────────────────────────────  ──────
   Blocked external 被报告成功                    0
  ──────────────────────────────────────────  ──────
   无进展后自由 ReAct 轮数                       ≤1
  ──────────────────────────────────────────  ──────
   可恢复操作恢复率                            100%
  ──────────────────────────────────────────  ──────
   不可判定结果进入 ManualReview               100%
  ──────────────────────────────────────────  ──────
   Golden Task A/B/C mandatory cases           100%
  ──────────────────────────────────────────  ──────
   Production completion 无 Harness receipt       0

  ## 五、实施顺序

  建议连续执行：

  P4-GPC0
  → GPC1
  → GPC2
  → GPC3
  → GPC4
  → GPC5
  → GPC6
  → GPC7
  → GPC8
  → GPC9
  → GPC10

  其中：

  - GPC0–GPC4 建立权威语义；
  - GPC5–GPC7 消除双轨绕过；
  - GPC8 证明纵向闭环；
  - GPC9–GPC10 完成运维、UI 和系统验收。

  ## 六、边界说明

  本批次不会：

  - 删除 legacy AgentLoop；
  - 把所有闲聊都强制送入昂贵五角色验收；
  - 宣称离线 Golden Tasks 等于 R6L 外部 Live；
  - 提前实施已暂缓的多节点 PostgreSQL/ObjectStore chaos；
  - 用测试通过数替代 false-completion 和 verified-completion 指标。

  本批完成后，项目最重要的变化应是：

  > Agent 可以停止，但只有 ClosureController 能宣布任务完成。

  如批准，我将按 P4-GPC0 → P4-GPC10 连续实施，测试通过后自动进入下一批。