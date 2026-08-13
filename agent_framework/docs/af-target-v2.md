复审结论：AF 这次有实质性跃迁，“推荐目标架构”的主干已经落地，但目前更准确的定位是：

> AgentTemplate 架构内核已完成，生产级多 Skill Agent Runtime 尚未完成接线与执行语义闭环。

我给出的综合判断：

| 范围 | 完成度 |
|---|---:|
| AgentTemplate 契约与数据模型 | 88–92% |
| 多 Skill 计划、校验、Session pin | 78–85% |
| 指令驱动模式 | 65–72% |
| 模型驱动模式 | 35–45% |
| Workflow 嵌入 | 65–72% |
| Runner SPI | 75–82% |
| Runner 生产后端 | 20–30% |
| 动态重规划与恢复 | 30–40% |
| 整体目标架构 | 68–75% |
| 可生产认证程度 | 45–55% |

## 一、这次真正完成了什么

新的独立主链已经形成：

```mermaid
flowchart LR
    A["AgentTemplateRegistry"] --> B["CandidateRetriever"]
    B --> C["SkillPlanProvider"]
    C --> D["PlanValidator"]
    D --> E["ActiveSkillSession"]
    E --> F["SkillWorkflowCompiler"]
    F --> G["SkillRunner"]
    G --> H["CompletionAuthority"]
    H --> I["Operations Projection"]
```

`AgentRuntime::run()` 已经把这些组件串联起来，包括：

1. 加载并校验 Template revision/digest。
2. 根据业务模式选择 PlanProvider。
3. 获取 SkillRegistry immutable snapshot。
4. 检索 Skill candidates。
5. 生成并确定性校验 `SkillCollaborationPlan`。
6. 构建并固定 `ActiveSkillSession`。
7. 持久化 Invocation。
8. 执行 Skill DAG。
9. 通过 CompletionAuthority 判定是否真正完成。

对应实现可见 [runtime.cpp](/home/Mapoet/projects/taskflow/agent_framework/src/agent_template/runtime.cpp:82)。

这已经不是原来 `AgentLoopNode` 中“只匹配一个 Skill，然后注入 prompt/policy”的简单扩展，而是新的 AgentTemplate 执行域。

另外，业务模式和承载模式已正确解耦：

- 业务模式：`ModelDriven`、`DirectiveDriven`、`Hybrid`、`FixedWorkflow`
- 承载模式：`Standalone`、`Conversation`、`WorkflowNode`、`WorkflowSubflow`、`RemoteA2A`

这是正确的架构方向。Workflow 节点和子流程适配器也已经存在，见 [runtime.cpp](/home/Mapoet/projects/taskflow/agent_framework/src/agent_template/runtime.cpp:195)。

## 二、测试结果

本轮直接执行了 14 个相关测试。

通过：

- 8 个新增 AgentTemplate 测试全部通过：
  - contracts
  - registry
  - session
  - planning
  - runner/compiler
  - runtime
  - governance
  - operations
- 原有 SkillRegistry、SkillCapabilityRuntime、SkillWorkflow、AgentLoop Skill policy 测试通过。

失败：

- `test_skill_process_sandbox`
- `test_skill_test_runner`

失败原因仍然是：

```text
no allowlisted interpreter for script type
```

这说明 AgentTemplate 新主链测试稳定，但脚本型 Skill 的执行准入环境仍未闭环。该失败不能简单归类为 AgentTemplate 回归，但它会直接限制生产 SkillRunner。

## 三、当前最主要的问题

### 1. 8 类 Runner 目前主要是类型和 SPI，不是 8 个生产 Runner

目前真正可见的通用实现只有：

- `SkillRunner` 接口
- `CallbackSkillRunner`
- `SkillRunnerRegistry`

见 [runner.hpp](/home/Mapoet/projects/taskflow/agent_framework/include/agent/agent_template/runner.hpp:10)。

没有发现默认产品 composition 中注册以下具体生产实现：

- ToolBus/LocalCapability runner
- SandboxedProcess runner
- CLI durable runner
- MCP runner
- ChildAgent runner
- NestedWorkflow runner
- HumanApproval runner
- InlinePrompt/AgentLoop runner

项目源码中也没有发现非测试代码创建 `AgentRuntime` 或注册生产 Runner。也就是说，目前流程基本是：

```text
AgentTemplate 核心可运行
        ↓
测试用 CallbackSkillRunner
        ↓
验证 DAG 和契约
```

还不是：

```text
AgentTemplate
  ├─ ToolBus
  ├─ MCP
  ├─ Sandbox
  ├─ ChildAgent/A2A
  ├─ Approval
  └─ Durable Workflow
```

因此文档中“8 类 runner `[x]`”更准确应写成：

> 8 类 Runner contract/SPI 已完成，生产 connector 尚未完成。

### 2. 模型驱动模式还没有真正接入模型

`ModelSkillPlanProvider` 本质上只是一个外部 callback：

```cpp
using ModelPlanCallback = std::function<SkillCollaborationPlan(...)>;
```

Provider 只调用 callback，并没有：

- 从候选 Skills 生成结构化 planner prompt；
- 调用 AF 的 LLMRuntime/Router；
- 使用 structured-output schema；
- 对 malformed plan 进行 repair；
- 记录模型、prompt revision、token、cost；
- 在产品入口安装默认 ModelPlanProvider。

见 [planning.cpp](/home/Mapoet/projects/taskflow/agent_framework/src/agent_template/planning.cpp:55)。

所以目前是“支持注入模型 planner”，还不是“AF 已提供 CC 式模型驱动多 Skill 协作”。

### 3. DirectiveDriven 的 Skill 分配算法过于简单

当前 `resolve_open_nodes()` 对未绑定节点的选择逻辑，本质是从候选集合里取第一个匹配候选 ID 的 Skill。

它没有在每个节点层面重新验证：

- `required_capabilities`
- SkillRole 适配度
- runner compatibility
- 权限适配度
- effect class
- cost/latency
- cardinality
- verifier/approver 特性

见 [planning.cpp](/home/Mapoet/projects/taskflow/agent_framework/src/agent_template/planning.cpp:23)。

而 `candidates_for()` 是先按 Template roles 分别取候选，再合并、去重。合并后节点解析可能拿到另一个 role 检索出来的 Skill。

建议改成：

```text
每个 SkillPlanNode
    → 构造独立 CandidateQuery
    → capability hard filter
    → permission/runner/effect hard filter
    → role compatibility scoring
    → deterministic tie-break
    → pin skill/version/digest
```

### 4. Compiler 没有执行完整的 Plan 语义

虽然 `SkillPlanNode` 已声明：

- `max_attempts`
- `failure_policy`
- `idempotency_key`
- `approval_required`
- `model_replannable`

边还声明了 `condition`，但 compiler 当前都没有真正消费这些字段。

当前算法只是：

1. 计算 DAG 入度。
2. 取 ready nodes。
3. 按 `max_parallelism` 分批。
4. `std::async` 执行 runner。
5. 任意节点失败就设置全局 cancel 并立即结束。
6. 收集 output/receipt。

见 [compiler.cpp](/home/Mapoet/projects/taskflow/agent_framework/src/agent_template/compiler.cpp:31)。

尚未实现：

- 条件边和分支；
- retry/backoff；
- continue/fallback/skip failure policy；
- node timeout；
- 幂等键查重；
- approval gate；
- verifier failure → replan；
- partial completion；
- checkpoint/attach/reconcile；
- output schema validation；
- artifact/evidence required 强制校验；
- committed effect receipt 写回。

因此当前是“有界并行 DAG executor”，还不是完整的 SkillWorkflowCompiler。

### 5. ReplanCoordinator 还没有接入 AgentRuntime 执行循环

`ReplanCoordinator` 已经实现了 revision CAS、parent digest 和 committed effect continuity 校验，这部分设计很好。

但 `AgentRuntime::run()` 当前只执行：

```text
propose → validate → execute → completion
```

没有：

```text
execute/observe
    → classify trigger
    → replan
    → rebuild or update session
    → resume unaffected nodes
    → execute revised plan
```

所以九类 replan trigger 目前是可调用的治理组件，并不是自动运行时行为。

### 6. Session 权限仍需要按节点收窄

每个 PinnedSkill 会计算：

```text
Skill manifest permissions ∩ AgentTemplate parent permissions
```

但最终：

```cpp
s.effective_permissions = r.parent_permissions;
```

见 [session.cpp](/home/Mapoet/projects/taskflow/agent_framework/src/agent_template/session.cpp:151)。

`RunnerRequest` 又携带整个 `ActiveSkillSession`。如果具体 Runner 只读取 Session 总权限，而没有再次与：

- 当前 Skill 的 pinned permissions
- 当前节点 requested permissions

取交集，就可能出现节点运行权限过宽。

建议显式生成：

```text
NodeEffectiveGrant
  = Template ceiling
  ∩ Invocation grant
  ∩ PinnedSkill grant
  ∩ SkillPlanNode requested grant
  ∩ Runtime policy
```

并把 `NodeEffectiveGrant` 作为 RunnerRequest 的一等字段。

### 7. `attach()` 默认退化成 `run()` 有副作用重复风险

当前接口默认实现：

```cpp
attach(request)    → run(request)
reconcile(request) → attach(request)
```

对于只读 prompt runner 尚可接受，但对：

- 文件写入
- CLI
- MCP 写操作
- ChildAgent
- 支付/消息/远端系统
- HumanApproval continuation

都可能造成重复执行。

更安全的语义是：

- 默认 `attach/reconcile` 返回 `unsupported`
- 只有实现 durable invocation identity 的 Runner 才允许 attach/reconcile
- effectful runner 必须通过 idempotency ledger 查询，而不能再次 `run()`

## 四、与目标业务模式的符合程度

### 指令驱动 Agent

已经具备基本骨架：

```text
AgentTemplate.workflow_skeleton
    → DirectiveSkillPlanProvider
    → resolve Skill
    → validate
    → compile DAG
```

但由于节点分配策略、条件、失败策略、approval、runner connector 尚未闭环，我判断为“可做离线声明式编排验证”，还不是完整业务模式。

### 模型驱动 Agent

当前完成的是 SPI 和治理边界：

```text
Model callback
    → SkillCollaborationPlan
    → deterministic validator
```

这是正确的安全结构，但模型 planner 本身尚未产品化，因此不能视为 CC 式模型驱动协作已经完成。

### Workflow 节点嵌入

`AgentTemplateNode` 和 `AgentTemplateSubflow` 已实现，方向正确。但尚缺：

- workflow durable context → invocation/session/checkpoint 的绑定；
- node cancellation 与内部 runner cancellation 的完整传播；
- workflow crash/restart 后 attach/reconcile；
- typed output ports，而不是只输出完整 `AgentRunResult`；
- Workflow UI 中对子 Agent/多 Skill 节点的分层追踪。

因此属于“适配器完成、durable orchestration 未完成”。

## 五、建议的下一阶段优先级

不建议继续扩展更多 contract。当前 contract 已足够，下一阶段应该集中做接线和行为闭环：

1. 实现并注册 8 类具体 Runner，优先复用现有 ToolBus、MCP、ChildTask/A2A、Approval、SkillWorkflow。
2. 建立 `ProductionAgentRuntimeBuilder`，所有 CLI/Web/TUI/GUI/Server 从同一 builder 获取 AgentRuntime。
3. 为模型驱动提供基于现有 LLMRuntime 的默认 structured planner。
4. 重写节点级 CandidateResolver，避免跨 role 错配。
5. 扩展 compiler：condition、retry、fallback、approval、idempotency、output/evidence validation。
6. 把 ReplanCoordinator 接入 observe-execute 循环。
7. 将 checkpoint/receipt/invocation 写入 durable store，并实现真实 attach/reconcile。
8. 修复脚本解释器 allowlist，使 Skill sandbox 与 Skill test runner 恢复通过。
9. 增加真实综合测试：
   - Model planner → MCP Skill → verifier
   - Directive planner → parallel Skills → synthesizer
   - approval suspend/restart/resume
   - ChildAgent failure → replan
   - workflow crash → attach/reconcile
   - effectful node idempotent replay

最终判断是：这次更新已经把 AF 从“多套 Skill 机制并存”推进到了“存在统一 AgentTemplate 架构内核”。这是决定性的进展。但现在最关键的工作不是再增加架构类型，而是让现有 ToolBus、MCP、AgentLoop、ChildAgent、Approval、SkillWorkflow 真正收敛到这条新主链中。

本轮只读审核，没有修改项目。工作区原有未跟踪目录 `agent_framework/docs/tools/`，我未触碰。