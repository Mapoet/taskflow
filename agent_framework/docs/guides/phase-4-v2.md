这个疑问非常关键。我会按代码解释方法核对当前 cognition、assurance、memory 三条链路里 LLM 的实际接入点、角色配置和控制权，区分“已
  经由 LLM 驱动”“只预留接口”“必须保持确定性”的部分。这里我使用 code-explainer skill，是为了把现有实现与目标架构逐层对照清楚。

• Explored
  └ Read SKILL.md (code-explainer skill), cognition_workflow.hpp, cognition_workflow.cpp, verifier.hpp, harness.hpp, harness.cpp,
         registry.cpp, view_engine.hpp, view_engine.cpp
    Search Verifier|verifier|CognitionModel|ModelProfile|model_profile|model_config|LLMProfile|profile.*model|planner|judge|
    reasoning
           in include
    Read verifier_runner.hpp
    Read verifier_runner.cpp
    Search struct .*Profile|class .*Profile|ModelConfig|LLMConfig|model_name|provider in agent
    Read verifier_runner.hpp
    Read verifier_runner.cpp
    Read types.hpp
    Read types.hpp
    Read agent_example_bootstrap.hpp
    Read agent_example_bootstrap.hpp
    Search AGENT_VERIFIER|VERIFIER_MODEL|VERIFIER_PROVIDER|SUMMARY_MODEL|summary_profile|ModelProfile in agent_framework
    Read memory_compaction.hpp, memory_compaction.cpp, governance.hpp, governance.cpp, runner.hpp, runner.cpp, phase-4.md

───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

• 核对结果明确：当前实现只是“为 LLM 驱动预留了部分接口”，尚不是您要求的完整 LLM 驱动规划—记忆—验证系统。尤其 Phase 4 Memory View 和
  Acceptance Arbiter 目前故意采用确定性逻辑；这部分应保留为安全控制面，但其上层的认知、记忆加工和专业验证工作流必须补入多角色 LLM。

• Explored
  └ Read cognition_workflow.hpp, cognition_workflow.cpp, harness.cpp, arbiter.cpp, view_engine.cpp, verifier_runner.cpp,
         memory_compaction.hpp, memory_compaction.cpp, types.hpp

───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

• 你的判断是正确的：规划、验证、记忆应该是 LLM 驱动工作流。当前 Phase 4 实现只具备部分 LLM 接口和确定性骨架，还没有形成完整的多角色
  LLM Runtime。

  ## 当前真实状态

  ### 1. 规划：只有一次 LLM 草拟接口

  当前 CognitionWorkflow：

  1. 确定性加载 Investigation View；
  2. 顺序调用 Investigator；
  3. 汇总 EvidenceBundle；
  4. 只调用一次 CognitionModel::draft()；
  5. 确定性验证 DAG；
  6. 保存计划。

  证据见 agent_framework/include/agent/planning/cognition_workflow.hpp:48 和 agent_framework/src/planning/
  cognition_workflow.cpp:38。

  缺少：

  - LLM 任务解释与事实缺口分析；
  - LLM 自主制定调查策略；
  - 多轮工具调查与证据反思；
  - 理解综合、技术路线比较、边界分析；
  - 独立 Planning Critic；
  - 新证据驱动的 LLM replan；
  - 真正的 LLMClient → CognitionModel 适配器。

  所以它目前是“有一个模型草拟插槽”，不是完整的 LLM 规划 Agent。

  ### 2. 验证：支持 Verifier 插件，但 Phase 4 尚无专业 LLM Verifier

  AssuranceHarness 会调度 Verifier，但当前测试使用的是确定性 verifier，没有注册 Code/Domain/Security 等 LLM 工作流，见
  agent_framework/src/assurance/harness.cpp:14。

  旧 Verifier 确实有独立模型设置：

  - AGENT_VERIFIER_MODEL
  - AGENT_VERIFIER_TEMPERATURE
  - 默认 temperature=0
  - 独立 timeout/retry/context budget

  见 agent_framework/src/agent/verifier_runner.cpp:60。

  但旧 Verifier 明确禁止工具和外部 API，只检查最终回答文本，不能承担专业验收。

  ### 3. 记忆：目前只有压缩环节由 LLM 驱动

  当前 Phase 4 Memory View 的 scope、ACL、authority、freshness、budget 和 conflict selection 都是确定性规则，见 agent_framework/
  src/memory_v2/view_engine.cpp:37。

  这部分保持确定性是正确的，因为 LLM 不能决定：

  - 是否绕过 tenant/ACL；
  - 是否把外部文本当系统指令；
  - 是否晋升 authoritative memory；
  - 是否覆盖 mandatory memory；
  - 是否泄露其他项目记忆。

  当前真正接入 LLM 的记忆能力主要是结构化对话压缩，已有独立 MemoryCompactionProfile，见 agent_framework/include/agent/agent/
  memory_compaction.hpp:53 和 agent_framework/src/agent/memory_compaction.cpp:255。

  但缺少 LLM 驱动的：

  - memory extraction；
  - 语义归一化和实体关联；
  - consolidation；
  - 冲突识别与解释；
  - 跨轮任务状态更新；
  - candidate memory 生成；
  - 动态 query expansion/reranking；
  - 遗忘和晋升建议。

  ## 正确架构：LLM 驱动认知，确定性系统掌握控制权

  LLM 分析/调查/综合/规划
            ↓
  确定性 Contract、Policy、CAS、权限和 DAG Gate
            ↓
  LLM/工具执行与反思
            ↓
  确定性 Artifact/Test/Metric Evidence
            ↓
  多角色 LLM 专业解释与交叉验证
            ↓
  确定性 Acceptance Arbiter

  LLM 应当深度参与“认知与判断”，但不能单独控制权限、持久状态和最终强制裁决。

  ## 不同工作流的建议模型设置

   LLM 角色                         模型要求                           建议参数                   权限
  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   Intake Analyst                   中高能力、结构化输出               temperature 0–0.2          无写权限
  ───────────────────────────────  ─────────────────────────────────  ─────────────────────────  ──────────────────────────────────
   Investigation Planner            最强推理模型                       high reasoning，0.1–0.3    只读 ToolBus、RAG、Web
  ───────────────────────────────  ─────────────────────────────────  ─────────────────────────  ──────────────────────────────────
   Evidence Synthesizer             长上下文、强推理                   high reasoning，0–0.2      只读 Evidence/View
  ───────────────────────────────  ─────────────────────────────────  ─────────────────────────  ──────────────────────────────────
   Architecture Planner             最强代码/系统模型                  high reasoning，0.1        只生成 plan candidate
  ───────────────────────────────  ─────────────────────────────────  ─────────────────────────  ──────────────────────────────────
   Planning Critic                  与 Planner 隔离，最好不同模型/     high reasoning，0          无写权限
                                    供应商
  ───────────────────────────────  ─────────────────────────────────  ─────────────────────────  ──────────────────────────────────
   Executor                         强工具调用模型                     0.1–0.3                    按 Plan/PDP 授权
  ───────────────────────────────  ─────────────────────────────────  ─────────────────────────  ──────────────────────────────────
   Memory Extractor                 中小型结构化模型                   0                          只生成 candidate
  ───────────────────────────────  ─────────────────────────────────  ─────────────────────────  ──────────────────────────────────
   Memory Consolidator              强推理、长上下文                   0–0.1                      不得自行晋升
  ───────────────────────────────  ─────────────────────────────────  ─────────────────────────  ──────────────────────────────────
   Conflict Resolver                强推理模型                         0                          只输出解释和建议
  ───────────────────────────────  ─────────────────────────────────  ─────────────────────────  ──────────────────────────────────
   Code/Domain/Security Verifier    独立强模型                         high reasoning，0          隔离 Verification View、只读工具
  ───────────────────────────────  ─────────────────────────────────  ─────────────────────────  ──────────────────────────────────
   Semantic Judge                   经校准模型                         0                          只能产生低等级 evidence
  ───────────────────────────────  ─────────────────────────────────  ─────────────────────────  ──────────────────────────────────
   Acceptance Arbiter               默认不用 LLM                       确定性规则                 无修改权限
  ───────────────────────────────  ─────────────────────────────────  ─────────────────────────  ──────────────────────────────────
   Eval Judge                       独立且盲测                         0、固定 seed/profile       不得看到 ground truth label

  关键任务应使用两个独立 verifier；至少不能让执行模型验证自己的产物。

  ## 需要新增统一的 LLM Role Profile

  当前 ModelConfig 只有 model、temperature、top_p、tokens、timeout、retry 和 extra_params，见 agent_framework/include/agent/core/
  types.hpp:160。它不足以描述生产级多角色工作流。

  建议新增 LLMRoleProfile：

  role: planning.architect
  provider_pool: [primary, fallback]
  model: reasoning-model
  reasoning_effort: high
  temperature: 0.1
  max_context_tokens: 128000
  max_output_tokens: 12000
  prompt_revision: planning-architect-v1
  output_schema: execution-plan-v1
  memory_view: planning
  capabilities:
    - repo_read
    - code_search
    - build_read
  independence_group: planner
  evidence_authority: candidate
  timeout_ms: 180000
  max_attempts: 2

  还必须记录每次实际调用的：

  - provider/model/version；
  - reasoning effort；
  - Prompt digest；
  - tool/capability manifest；
  - MemorySnapshot/View digest；
    -输入/输出 canonical digest；

  - token、成本、延迟；
  - fallback 和重试路径；
  - independence group；
  - eval/calibration revision。

  ## 应如何调整后续 Phase 4

  应该在继续 P4-F3I/P4-F4A 前插入一个 P0 横向能力：P4-F1L Role-based LLM Workflow Runtime，然后分别完成：

  1. LLM Profile Registry、Model Router 和调用审计；
  2. 多阶段 Cognition Agent；
  3. LLM Memory Extraction/Consolidation/Conflict Workflow；
  4. 多角色专业 LLM Verifier；
  5. LLM remediation→replan；
  6. LLM judge calibration/evaluation；
  7. 确定性 policy/arbiter 对所有 LLM 输出进行 fail-closed gate。

  此外，“深度思考”不应持久化模型私有 chain-of-thought；应持久化可审计的 evidence、assumptions、alternatives、decision rationale、
  unknowns、risks 等专业推理产物。这样既保留认知深度，也保证系统可验证、可重放。