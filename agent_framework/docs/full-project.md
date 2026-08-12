结论先说：`agent_framework` 已经从“基于 Taskflow 的 ReAct Agent”演化成一个相当完整的 Agent 生产运行时原型。它最强的部分是确定性控制面、持久化契约、审批/验收/恢复设计；当前最大的短板不是功能少，而是功能面过宽、旧循环与新 Harness 双轨并存、完成判定尚未完全统一。

若目标是“任务收敛与快速闭环”，下一阶段不应继续横向增加模块，而应把所有生产入口压缩到一条不可绕过的纵向路径：

> Intake → Contract → Plan → Execute → Verify → 最小修复 → Reverify → Terminal

## 1. 目录的真实含义

用户提供的 [include.txt](/home/Mapoet/projects/taskflow/agent_framework/include.txt) 是公共头文件快照，但只能说明“声明了哪些能力”，不能单独证明实现和运行成熟度。

当前规模约为：

| 项目 | 数量 |
|---|---:|
| `include/agent` 公共头文件 | 199 |
| `src` 实现文件 | 234 |
| `tests/test_*.cpp` | 197 |
| CMake `add_test` 声明 | 198 |

从职责上可以分为六层：

1. 基础执行层  
   `llm_client`、`toolbus`、`mcp_client`、`sandbox`、`vectorstore`、`resources`。

2. Agent 认知层  
   `planning`、`memory_v2`、`skills`、`prompt_renderer`、`llm_runtime`。

3. 工作流与调度层  
   `graph_executor`、`node/agent_loop_node`、`child_task`、`a2a`。

4. 确定性控制层  
   `contracts`、`run`、`approval`、`harness`、`assurance`、`remediation`。

5. 生产治理层  
   `distributed`、`live`、`eval`、`telemetry`、`observability`。

6. 交互层  
   CLI、TUI、Web、ImGui 及同源 Operations projection。

它本质上已接近：

> LLM cognitive plane + deterministic execution/control plane + durable evidence plane

而不再只是一个 Agent SDK。

## 2. 当前最核心的两条执行路径

### 旧路径：AgentLoop

旧路径以 `AgentLoopNode` 为中心：

```text
Prompt/Memory/RAG
      ↓
     LLM
      ↓
Tool calls → ToolBus → observations
      ↑                    │
      └────────────────────┘
      ↓
 final_answer / is_final
```

它已经具备：

- 相同 `tool_call_id` 的副作用防重放；
- 单轮工具调用数量限制；
- 只读工具并行；
- A2A submit 并行；
- 取消、deadline 和上下文预算；
- 同轮重复工具调用保护；
- 最大迭代次数。

这些机制具有较好的“防失控”能力。例如：

- 已提交工具结果不会在恢复时重复执行：[agent_loop_node.cpp:741](/home/Mapoet/projects/taskflow/agent_framework/src/node/agent_loop_node.cpp:741)
- 重复工具调用会触发保护：[agent_loop_node.cpp:819](/home/Mapoet/projects/taskflow/agent_framework/src/node/agent_loop_node.cpp:819)
- 默认最大 10 轮、每轮 5 个工具：[types.hpp:398](/home/Mapoet/projects/taskflow/agent_framework/include/agent/core/types.hpp:398)

但它的“完成语义”仍然偏弱：

```cpp
tool_calls.empty() &&
(llm_out.is_final || !llm_out.final_answer.empty())
```

也就是说，只要模型不再调用工具，并输出了非空答案，就可能被视为完成：[agent_loop_node.cpp:1043](/home/Mapoet/projects/taskflow/agent_framework/src/node/agent_loop_node.cpp:1043)。

循环条件随后只检查这个 `is_final`：[agent_loop_node.cpp:1064](/home/Mapoet/projects/taskflow/agent_framework/src/node/agent_loop_node.cpp:1064)。

因此它解决的是：

- 不无限循环；
- 不重复调用；
- 能及时返回；

但没有充分解决：

- 任务是否真正完成；
- 产物是否存在；
- 验收标准是否满足；
- 证据是否足够；
- 到达上限时究竟是成功、失败还是未决。

这是“停止”而不是严格意义上的“收敛”。

### 新路径：Phase 4 Harness

新 Harness 的目标路径是：

```text
Intake
  → Cognition / Planning
  → Plan Approval
  → Execution
  → Memory Update
  → Professional Assurance
      ├─ Accepted → Judge / Operations → Completed
      └─ Finding  → Remediation Approval
                    → Re-execution
                    → Artifact Rebind
                    → Selective Re-verification
```

这一方向是正确的，因为它把“完成”从模型陈述变成了确定性控制面的结论：

- AcceptanceContract 定义完成条件；
- ArtifactManifest 绑定真实产物；
- 强 oracle 检查文件、构建、测试、指标；
- ApprovalStore 管理有副作用操作；
- Remediation 只修复未满足项；
- Reverification 检查新产物摘要；
- Run/Harness Store 保证恢复与防重放。

项目自身也明确规定：没有直接证据就不能标记完成，[phase-4-status.md:10](/home/Mapoet/projects/taskflow/agent_framework/docs/guides/phase-4-status.md:10)。

这是整个项目最有价值的设计。

## 3. 当前成熟度评价

我的审计评价是：

| 维度 | 评价 |
|---|---|
| 架构完整性 | 很强 |
| 确定性控制面 | 很强 |
| 持久化与恢复 | 强 |
| 测试广度 | 很强 |
| 生产默认路径统一性 | 中等 |
| 真实外部认证 | 偏弱 |
| 文档一致性 | 中等偏弱 |
| 快速交付效率 | 当前偏弱 |
| 继续扩展的边际收益 | 已明显下降 |

项目状态文档给出的当前判断是：

- v2 工程实现成熟度约 84–87%；
- 生产认证成熟度约 58–64%；
- Phase 4 总体仍为 `[~]`。

详见 [phase-4-status.md:21](/home/Mapoet/projects/taskflow/agent_framework/docs/guides/phase-4-status.md:21)。

这个判断基本合理。但同一文档中 v1 成熟度同时出现 `77–81%` 和 `76–80%`，[phase-4-status.md:30](/home/Mapoet/projects/taskflow/agent_framework/docs/guides/phase-4-status.md:30)、[phase-4-status.md:36](/home/Mapoet/projects/taskflow/agent_framework/docs/guides/phase-4-status.md:36)，说明状态统计本身已有轻微漂移。以后不宜继续依赖主观百分比，应改用可计算的门禁矩阵。

## 4. 影响收敛的主要问题

### 4.1 双轨完成语义

旧 AgentLoop 可以直接产生 `final_answer`；新 Harness 则要求 Contract、Artifact、Evidence、Oracle、Approval 全部闭合。

如果 CLI/Web/A2A 等入口仍能绕过 Harness，项目就拥有两种互相不等价的“完成”：

- `MODEL_FINAL`
- `VERIFIED_COMPLETE`

生产入口必须只承认第二种。

### 4.2 终止、失败和完成混在一起

当前重复工具保护会设置 `is_final=true`。达到 guard、超时、模型异常也会终止循环，但这些并不是任务成功。

应把布尔值升级为终态枚举：

```text
COMPLETED_VERIFIED
COMPLETED_WITH_LIMITATIONS
NEEDS_USER_INPUT
BLOCKED_EXTERNAL
BUDGET_EXHAUSTED
STAGNATED
FAILED_EXECUTION
FAILED_VERIFICATION
MANUAL_REVIEW
CANCELLED
```

所有终态必须携带：

- `reason_code`
- `unsatisfied_criteria`
- `evidence_refs`
- `last_progress_revision`
- `resume_token`
- `recommended_next_action`

### 4.3 缺少跨轮停滞检测

现有重复调用保护只覆盖“同一轮相同工具+相同参数”，并不能识别：

- 连续多轮没有新增证据；
- 计划 revision 变化但内容等价；
- 工具参数微调但结果摘要相同；
- 修复后仍产生相同 finding；
- token/cost 持续增长但 acceptance coverage 不增长。

应定义确定性的进展函数：

```text
progress =
  新关闭验收项数量
+ 新增有效证据数量
+ 新生成产物摘要数量
+ finding 严重度下降
- 新增 blocker
- 重复 effect
```

若连续两轮 `progress <= 0`，禁止继续自由 ReAct，转入：

1. 重新规划一次；
2. 仍无进展则 `NEEDS_USER_INPUT`、`BLOCKED_EXTERNAL` 或 `MANUAL_REVIEW`。

### 4.4 工程面过宽

当前项目同时维护：

- 多模型适配；
- MCP/A2A；
- Skills 供应链；
- 两套 Memory；
- 多端 UI；
- Eval/Judge；
- Live certification；
- SQLite/PostgreSQL/HA；
- Sandbox/OTLP/SLO；
- Cognition/Assurance/Remediation。

这种广度对平台研究很有价值，但不利于快速形成一个稳定产品闭环。下一阶段应冻结横向扩张，集中在一条 golden path。

### 4.5 声明能力与可运行能力仍有差距

源码中还存在明确缺口，例如：

- `Workflow builder` 尚为 TODO；
- `Event logger` 尚为 TODO；
- `register_api_tool` 直接抛出未实现；
- Gemini、vLLM adapter 仍抛 `not implemented`；
- WebSocket MCP transport 尚未实现；
- 多模态序列化仍有 base64 和时间解析 TODO。

因此 README 中“多模态”“WebSocket”“多 provider”的表述，需要区分：

- 已稳定实现；
- 部分实现；
- 契约存在；
- 尚未实现。

README 的目录描述也明显落后于实际工程规模，[README.md:93](/home/Mapoet/projects/taskflow/agent_framework/README.md:93)。

## 5. 推荐的快速闭环架构

建议新增一个统一的 `TaskClosureController`，成为所有生产入口的唯一完成判定器：

```text
User/A2A/API
    ↓
TaskIntake
    ↓
AcceptanceContract
    ↓
Bounded Plan
    ↓
Execute one smallest actionable step
    ↓
Collect Artifact + Evidence
    ↓
Closure Evaluator
    ├─ all mandatory criteria satisfied → COMPLETED_VERIFIED
    ├─ fixable findings → minimal remediation
    ├─ missing facts → NEEDS_USER_INPUT
    ├─ external dependency → BLOCKED_EXTERNAL
    ├─ no progress → STAGNATED / MANUAL_REVIEW
    └─ budget exceeded → BUDGET_EXHAUSTED
```

Closure Evaluator 应完全确定性，不由 LLM 最终决定。LLM负责：

- 理解；
- 调查；
- 规划；
- 解释；
- 提出修复方案。

确定性控制面负责：

- 是否允许执行；
- 证据是否有效；
- 是否满足验收条件；
- 是否继续消耗预算；
- 最终属于哪个终态。

## 6. 最值得优先完成的五项工作

### P0：统一终态与 Completion Gate

把所有入口最终汇入 Harness：

- CLI；
- Web；
- AgentServer；
- A2A；
- GraphExecutor；
- Skill Workflow。

`AgentLoopNode::is_final` 只表示“本轮模型不再要求工具”，不得直接表示任务完成。

### P1：AcceptanceContract 前置

每个非闲聊任务在执行前生成最小验收合同：

```json
{
  "deliverables": [],
  "mandatory_criteria": [],
  "verification_methods": [],
  "allowed_side_effects": [],
  "budget": {},
  "clarification_policy": {},
  "completion_policy": {}
}
```

简单任务可使用轻量合同，复杂任务使用完整专业 DAG，避免所有请求都进入昂贵 Phase 4 全链路。

### P2：引入进展账本和停滞检测

每轮持久化：

- 关闭了哪些 criteria；
- 新增了哪些 evidence/artifact；
- findings 如何变化；
- 消耗了多少 token、工具调用和时间；
- 下一步为何仍有信息增益。

没有新增信息就不允许重复“思考—调用—观察”。

### P3：验证驱动的最小修复

不要重新执行整个计划。由 finding 映射到：

```text
finding
  → impacted criteria
  → impacted artifacts
  → minimal approved action
  → invalidated evidence
  → selective reverification
```

项目现有 residual plan 已提出这一方向，[phase-4-residual-closure-plan.md:74](/home/Mapoet/projects/taskflow/agent_framework/docs/guides/phase-4-residual-closure-plan.md:74)，应把它提升为生产默认行为。

### P4：建立三个 Golden Tasks

停止继续用大量平行 fixture 表达成熟度，固定三个真正贯穿全链路的任务：

1. 文件交付任务  
   创建文件→验证路径、内容、摘要→完成。

2. 代码修复任务  
   定位问题→批准修改→构建/测试→故意失败→最小修复→复验。

3. 外部依赖任务  
   调查→遇到无凭据/服务离线→正确进入 `BLOCKED_EXTERNAL`，不能假成功。

每个任务必须覆盖：

- 正常完成；
- 用户补充信息；
- 取消；
- 超时；
- crash/restart；
- 重复投递；
- 副作用幂等；
- 验收失败；
- 修复循环上限。

## 7. 建议的任务收敛指标

以后不要只报告测试通过数，建议长期跟踪：

| 指标 | 目标 |
|---|---:|
| Verified completion rate | >95% |
| False completion rate | 0 |
| 首次验收通过率 | 按任务类型建立基线 |
| 平均修复轮数 | ≤1.5 |
| 无进展轮数 | ≤1 |
| 重复工具 effect | 0 |
| Crash 后已确认 effect 重放 | 0 |
| 用户本可一次补全却多轮追问 | 持续下降 |
| `BLOCKED` 被误报为成功 | 0 |
| 每关闭一个 criterion 的 token/tool 成本 | 持续下降 |

“快速闭环”不等于更少迭代，而是每次迭代都关闭一个明确缺口。

## 8. 工作区现状提醒

当前分支为 `phase-4`，工作区已有用户未提交修改：

- 修改：[CMakeLists.txt](/home/Mapoet/projects/taskflow/agent_framework/CMakeLists.txt:312)
- 新增：`cross_store_coordination.hpp/.cpp`
- 新增：`include.txt`

`include.txt` 也没有列出刚新增的 `cross_store_coordination.hpp`，所以它已经不是完整的实时目录事实源。

本轮没有修改文件，也没有重新运行测试。`70/70 phase4-offline PASS` 是项目状态文档记录的既有结果，不是本轮重新验证的结果。

最终评价：这是一个技术深度很高、控制面设计明显优于常见 Agent 框架的工程，但现在已进入“收敛优先于扩张”的阶段。最优路线不是继续增加模块，而是让 AcceptanceContract 和 ClosureController 取代 `is_final`，成为所有生产入口唯一的任务完成权威。