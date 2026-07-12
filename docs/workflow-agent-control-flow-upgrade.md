# Workflow 与 Agent Framework 控制流升级清单

## 结论

Taskflow 4.x 合并后的编译与离线测试最初只证明了 API 兼容性。本文件列出的控制流
升级已于 2026-07-12 实施，当前实现同时覆盖两条主线：

1. 升级前的 `create_loop_decl` 已使用 Taskflow 条件任务的回边，但循环体、条件、出口之间的
   状态传递、输出生命周期和再次运行仍由 Workflow 自定义实现，未形成稳定的
   Taskflow 4.x 条件循环契约。
2. 升级前的 `create_subgraph` 和 `create_subtask` keyed 输入/输出重载返回占位值，无法作为
   可组合、可重入的模块边界。该限制会直接传导到 Agent Framework 的多轮工具调用、
   子 agent、恢复执行和嵌套工作流。

验收仍按“同一个图、同一个子图和同一个 agent session 能否在明确的状态版本与输出
所有权下安全执行多次”执行，不能只看编译结果。

## 实施状态

| 清单 | 状态 | 实现证据 |
| --- | --- | --- |
| A1-A3 循环状态、入口/回边、输出槽 | 已完成 | `create_loop`、`AnyValueSlot`、`LoopResult`；0/1/2/10/1000 次和三次重跑测试 |
| A4 终止、异常、取消 | 已完成 | 七类 `LoopStatus`，body/condition/exit error、cancel、deadline 测试 |
| B1-B2 显式端口和模块类型 | 已完成 | `OutputBindings`、`SubflowModule`、`create_subgraph_module`、`create_subtask_module` |
| B3 实例身份和重启策略 | 已完成 | `RunContext` 与 `SubflowStartMode::{Retry,Restart,Resume}`；attempt 隔离测试 |
| B4 三层嵌套传播 | 已完成 | parent/child/grandchild depth 与 run identity 测试 |
| C1 Agent 显式状态流 | 已完成 | `AgentLoopNode` body 输出 feedback 到下一轮，无跨回调共享可变状态 |
| C2 session 提交与恢复 | 已完成 | `MergeReactSessionMode::ResumeFromCheckpoint` 幂等合并测试 |
| C3 受控 subflow | 已完成 | `SubflowNode`、结构化 result/usage、深度/迭代/工具/deadline/cancel 预算 |
| D1-D3 文档 | 已完成 | Workflow README、Key-Based API、本指南、Agent Loop 指南与升级报告 |

新增 CTest 标签为 `workflow-control-flow`、`workflow-module`、`agent-loop` 和
`agent-subflow`。最终全量测试结果记录在 `docs/upstream-upgrade.md`。

## 原始风险与关闭状态

下表保留升级前的定位证据，便于回溯问题来源；R1-R8 均已由上方实施项关闭，行号不再
代表当前实现位置。旧 keyed API 现在显式拒绝不完整调用，不再静默返回空值。

| ID | 优先级 | 证据 | 风险判断 |
| --- | --- | --- | --- |
| R1 | P0 | `workflow/src/nodeflow.cpp:518-557` | keyed `create_subgraph` 在构图期向 builder 传入空 `std::any`，运行后也只返回空占位输出；声明的输入/输出契约并未实现。 |
| R2 | P0 | `workflow/src/nodeflow.cpp:595-629` | keyed `create_subtask` 虽在执行期构建并 `corun` 子图，但没有收集嵌套图输出。循环体无法可靠地把本轮结果交给 condition 或下一轮。 |
| R3 | P0 | `workflow/src/nodeflow.cpp:744-903` | loop wrapper 依靠 `body -> condition -> body/exit`、外部前驱和 Taskflow join counter 共同启动；源码注释已记录“执行两次后不再调度”的未决风险。 |
| R4 | P0 | `workflow/src/nodeflow.cpp:929-1001` | loop 输出使用单次 `std::promise`；循环体或条件第二次对同一 key `set_value` 会违反 promise 的一次性写入语义，同一图再次运行也无法复用。 |
| R5 | P1 | `workflow/src/nodeflow.cpp:769-840` | `static` 调试计数器和直接写 `std::cout` 会跨图共享并产生并发数据竞争，不应存在于运行时控制流实现。 |
| R6 | P0 | `agent_framework/src/node/agent_loop_node.cpp:154-158` | Agent loop 明确依赖闭包共享状态，因为 Workflow condition 收不到 body 输出；图的数据依赖在调度器之外，难以检查、恢复和并发隔离。 |
| R7 | P1 | `agent_framework/src/node/agent_loop_node.cpp:713-803` | `iteration`、`is_final` 和 `next_agent_state` 已有明确业务语义，但没有成为 Workflow loop 的显式迭代状态，只能由 Agent Framework 自行归约和读取。 |
| R8 | P1 | `agent_framework/include/agent/graph_executor.hpp:162-182` | session merge 已区分 `FullUserTurn` 与 `DeltaOnly`，但图重启、失败重试、从 checkpoint 恢复时的 exactly-once 边界尚未定义。 |

## 已完成的更新列表

### A. 循环语义与生命周期

#### A1. 定义唯一的循环状态契约

- **类型：实现升级 + API 文档升级**
- **优先级：P0**
- 引入显式 `LoopState`/`IterationContext`，至少包含本轮输入、本轮输出、迭代号、
  `continue/exit/error/cancel` 状态和可选 checkpoint 标识。
- condition 应读取本轮 body 的实际输出，而不是只读取循环启动前创建的
  `input_futures`；下一轮 body 应读取上一轮提交后的状态。
- 固定计数语义：`iteration` 表示已完成的 body/LLM 调用次数，body 成功提交后递增，
  condition 在递增后判断 `max_iterations`。
- 保留旧 `create_loop_decl` 重载作为兼容层，并标记为 legacy；新接口不应继续增加
  `body_func`、`body_task`、`body_builder_fn` 等平行重载。
- **验收标准：** condition 能直接观察第 N 轮 body 输出；测试不得依赖 lambda 外部
  可变变量证明循环成立。

#### A2. 将“首次启动”与“回边重调度”分离

- **类型：实现升级 + 内部架构文档升级**
- **优先级：P0**
- 使用单独的 loop-entry task 处理外部前驱，只允许它触发第一次 body；后续迭代仅由
  condition 的 back-edge 触发，避免 body 同时长期保留 entry 与 condition 两类前驱。
- 对照 Taskflow 4.x conditional task 的 successor index 语义定义：`0 = continue`、
  `1 = exit`，非法 index、空出口、取消和异常必须有确定行为。
- 删除依赖 join counter 内部行为的推断性代码注释；Workflow 只能依赖 Taskflow 公共
  条件任务语义。
- **验收标准：** 0、1、2、10、1000 次迭代均精确执行；入口源节点只执行一次；
  body、condition 和 exit 的调用次数可断言。

#### A3. 重做循环输出存储

- **类型：实现升级**
- **优先级：P0**
- 不得把单次 `std::promise` 当作跨迭代变量。每轮使用独立结果槽，或由循环控制器在
  condition 完成后原子提交一个新的 immutable state snapshot。
- 对外输出只在 exit/error/cancel 终态提交一次；中间结果通过 iteration state 传递，
  不写最终输出 promise。
- 明确同一 `GraphBuilder`/Taskflow topology 是否支持 `run_n`、连续 `run().wait()` 和
  并发 run。若不支持，API 必须拒绝并给出诊断，不能隐式复用失效 promise。
- **验收标准：** 同一已构建图连续运行 3 次结果互不污染；TSAN 模式下无共享写竞争。

#### A4. 补齐终止、异常与取消传播

- **类型：实现升级 + API 文档升级**
- **优先级：P1**
- 终态至少区分 `completed`、`max_iterations`、`cancelled`、`deadline_exceeded`、
  `body_error` 和 `condition_error`。
- 定义 body 部分执行后失败时是否提交本轮状态；推荐默认不提交，重试使用相同
  iteration id。
- exit handler 必须恰好执行一次；取消和异常路径是否执行 cleanup 应由选项明确控制。
- **验收标准：** 每种终态都有确定输出和单元测试，异常不会被 `corun` 边界吞掉。

### B. 可复用子图与子任务

#### B1. 建立显式模块端口，不再猜测嵌套节点输出

- **类型：实现升级 + 公共 API 升级**
- **优先级：P0**
- 为子图定义显式 input/output ports，例如 `SubgraphInterface` 或
  `ModuleResult`；builder_fn 必须把模块输出 key 绑定到嵌套图内具体的
  `{node, output_key}`。
- 父图只消费已声明并完成类型检查的输出；缺失 key、重复 key 和 `std::any` 类型不符
  应在构图期或模块完成时明确报错。
- 不再返回空 `std::any` 占位。旧 keyed 重载在真正实现前应在运行时抛出
  `logic_error`，避免产生“执行成功但输出为空”的静默错误。
- **验收标准：** 两输入、两输出子图可被父图 sink 消费；缺失输出测试必须失败而非
  返回空值。

#### B2. 区分静态 module 与动态 subtask

- **类型：API 设计升级 + 文档升级**
- **优先级：P0**
- `create_subgraph` 定义为构图期创建、生命周期随父图、适合静态
  `composed_of` module。
- `create_subtask` 定义为执行期实例化、每次调用具有独立 context/result、适合循环体、
  动态工具批次和子 agent。
- 两者应共享同一端口协议，但不能共享运行态 promise、取消令牌和可变输出缓存。
- 文档明确 nested `corun` 的线程占用、异常传播、父 executor 复用和禁止场景。
- **验收标准：** 同一 module 多次调用与两个 module 并发调用均无状态串扰。

#### B3. 增加子任务实例身份和重启策略

- **类型：实现升级 + Agent API 升级**
- **优先级：P1**
- 每次动态子任务分配稳定的 `parent_run_id / subtask_id / attempt / iteration`，供日志、
  checkpoint、幂等工具调用和结果归并使用。
- 定义三种操作：`retry` 重跑当前 attempt、`restart` 从模块入口创建新 attempt、
  `resume` 从已提交 checkpoint 继续。三者不能只靠再次调用 `corun` 隐式实现。
- 输出采用 attempt 级隔离；只有成功提交的 attempt 才能替换父图可见结果。
- **验收标准：** body 在第 3 轮失败后可重试且前两轮不重复提交；恢复后工具调用不会
  被无条件执行两次。

#### B4. 支持嵌套模块的结构化传播

- **类型：实现升级 + 文档升级**
- **优先级：P1**
- 父取消令牌、deadline、trace context 和资源预算必须向子图递归传播；子图不得只从
  `GraphBuilder::executor_` 获取隐式运行环境。
- 规定 child error 到 parent 的映射和聚合策略，避免嵌套 `corun` 只留下普通异常文本。
- 增加最大嵌套深度或递归检测，防止 agent 自生成 subflow 形成无界递归。
- **验收标准：** parent -> child -> grandchild 三层测试覆盖成功、取消、超时和异常。

### C. Agent Framework 跟进

#### C1. 将 AgentLoopNode 改为显式状态流

- **类型：实现升级**
- **优先级：P1，依赖 A1-A3 与 B1**
- `StateMerge` 输出直接绑定到 Workflow `LoopState.next_inputs`，condition 读取
  `is_final`、`iteration` 和 tool-call 状态，不再通过 `shared` 闭包旁路传递。
- 闭包仅保存不可变依赖，如 client、registry 和 config；session state 必须由图端口传递。
- `next_agent_state` 只在 loop exit 时作为最终输出发布。
- **验收标准：** 两个 agent loop 在同一 executor 并发运行时 session、iteration 和
  final answer 完全隔离。

#### C2. 定义 agent 运行、重试和恢复的提交边界

- **类型：实现升级 + Agent 文档升级**
- **优先级：P1**
- 把 `merge_react_session_state` 定义为一次成功 run 的 commit；失败或取消的 attempt
  不得部分追加 history。
- `FullUserTurn`、`DeltaOnly` 与新增的 `ResumeFromCheckpoint` 行为应形成状态转移表，
  明确 user message、assistant message、tool result 和 iteration 的去重规则。
- 工具调用使用稳定 `tool_call_id + attempt` 做幂等键；具有外部副作用的工具默认不得
  自动重试。
- **验收标准：** retry/resume 后 history 无重复消息，副作用工具调用次数可断言。

#### C3. 开放受控的 subflow/sub-agent 接口

- **类型：Agent API 升级 + 文档升级**
- **优先级：P2，依赖 B2-B4**
- 提供 `SubAgentNode` 或 `SubflowNode`，输入至少包含任务、父 context、预算和终止策略，
  输出使用结构化 result/error/usage，而不是直接共享父 agent 内存。
- 默认限制最大深度、总迭代数、总工具调用数和 deadline；并行 fan-out 需要独立预算。
- 第一阶段只支持静态注册的 subflow 模板；动态生成任意图应后置。
- **验收标准：** 主 agent 调用两个子 agent 后可确定性聚合，且一个子任务失败不会破坏
  另一个子任务的已提交结果。

### D. 文档升级

#### D1. 修订 Workflow API 文档

- **类型：纯文档，必须与 A/B 实现同一提交或紧随其后**
- **优先级：P0**
- 为 `create_loop_decl` 给出唯一推荐模式、状态时序、successor index、首次启动、退出、
  异常、取消和图再次运行语义。
- 为 `create_subgraph`/`create_subtask` 给出静态模块与动态实例对照表、端口声明示例和
  嵌套执行限制。
- 在 keyed 输出实现完成前，明确标记当前接口为 incomplete/experimental。

#### D2. 修订 Agent Loop 指南

- **类型：纯文档，依赖 C1-C2**
- **优先级：P1**
- 更新 `agent_framework/docs/guides/phase-1-wp5.md`：删除“condition 直接读取 body outputs”
  与当前实现不一致的叙述，并在新 Workflow API 完成后改为显式 `LoopState` 示例。
- 补充多轮工具调用、达到上限、取消、重试、恢复、并行 session 和 sub-agent 的状态图。
- 将共享闭包方案标为兼容实现，不再作为推荐架构。

#### D3. 更新升级验证报告

- **类型：纯文档**
- **优先级：P1**
- 在 `docs/upstream-upgrade.md` 中区分“4.x 编译/现有测试通过”和“控制流语义验证通过”。
- 后者只有在下面的测试矩阵完成后才能标绿，避免现有 2984 个测试通过掩盖 keyed
  子图输出和循环重入未覆盖的问题。

## 必须新增的回归测试矩阵

| 测试组 | 最小用例 | 验证目标 |
| --- | --- | --- |
| Loop cardinality | 0、1、2、10、1000 次 | 无少跑、多跑和两次后停滞；entry/exit 次数正确。 |
| Loop dataflow | `state[n+1] = f(state[n])` | body 输出确实进入 condition 和下一轮，而非闭包偶然可见。 |
| Re-run | 同一图串行运行 3 次 | promise、iteration、输出和错误不跨 run 污染。 |
| Concurrent runs | 同一 executor 上两个图并发 | 无 static/shared closure 串扰；TSAN 无竞争。 |
| Keyed subgraph | 多输入、多输出、缺失 key、错误类型 | 端口绑定和错误诊断真实有效。 |
| Dynamic subtask | 同一 subtask 循环实例化 10 次 | 每次独立输出、析构和异常传播。 |
| Nested subflow | 三层成功/异常/取消/超时 | context 和终态逐层传播。 |
| Retry/restart/resume | 中途失败后分别执行三种策略 | attempt 隔离、checkpoint 和 exactly-once 提交。 |
| Agent ReAct | mock LLM 连续 3 轮 tool call 后 final | history、iteration、tool_call_id 和 final output 正确。 |
| Agent sessions | 两个 session 并行且一个取消 | 状态隔离，取消不影响另一 session。 |

建议为上述测试设置独立 CTest label：`workflow-control-flow`、`workflow-module`、
`agent-loop` 和 `agent-subflow`，使 4.x 后续合并可以单独运行语义回归，而不必依赖全量
Taskflow 核心测试发现问题。

## 推荐实施顺序

1. **P0-1：** 先新增失败测试，稳定复现 keyed 输出占位、循环多次迭代和图重跑问题。
2. **P0-2：** 实现显式模块端口与每次运行/每次迭代的结果槽，完成 B1、A3。
3. **P0-3：** 重构 loop entry/back-edge 和 `LoopState`，完成 A1、A2。
4. **P0-4：** 收敛 `create_loop_decl`、`create_subgraph`、`create_subtask` API，并提供旧接口兼容层。
5. **P1-1：** 迁移 `AgentLoopNode`，移除共享可变闭包状态，完成 C1。
6. **P1-2：** 增加 retry/restart/resume、取消和 checkpoint 语义，完成 A4、B3、C2。
7. **P1-3：** 补齐嵌套 context 传播和全部文档，完成 B4、D1-D3。
8. **P2：** 在底层契约稳定后再开放通用 `SubflowNode`/`SubAgentNode`，完成 C3。

## 本轮升级完成定义

以下条件全部满足后，才能把 Workflow/Agent Framework 标记为“已完整适配 Taskflow 4.x
控制流”：

- keyed 子图和子任务返回真实、可验证的输出，不存在空 `std::any` 占位路径；
- condition 直接消费本轮 body 输出，Agent Framework 不依赖共享可变闭包维持循环；
- 循环 cardinality、同图重跑、并发运行和三层嵌套测试全部通过；
- retry、restart、resume、cancel 和 error 的状态转换有文档与测试；
- 现有 Agent Framework 离线测试继续通过，新增控制流 CTest labels 全部通过；
- Workflow README、API 注释和 Agent Loop 指南与实际实现一致。

## 2026-07-12 验收证据

- Debug 全量构建成功，Workflow、Taskflow CPU 与 Agent Framework 全部目标完成编译。
- 语义标签测试 `5/5` 通过；兼容修复后核心与 WP2.7 回归集 `6/6` 通过。
- 全量 CTest 覆盖 `2989` 项：一次并行运行通过 `2988` 项，剩余
  `agent_client_a2a` 在同次调度中长时等待；隔离重跑于 26.46 秒通过。此前发现的
  `a2a_contract_loopback` 并行端口/时序冲突已通过 `RUN_SERIAL` 修复并在全量调度中通过。
- TSAN 下 `workflow_runtime_slots`、`workflow_modules`、`workflow_control_flow` 为 `3/3`
  通过，无数据竞争报告。
- `control_flow_v4` 示例输出 `value=3`；占位实现、静态循环计数器和调试注释扫描无命中；
  `git diff --check` 通过。
