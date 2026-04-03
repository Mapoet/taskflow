# 深度分册：技术流与并行

## 1. agent_framework（AF）

### 1.1 构图与 Loop

CLI ReAct 图在 `build_cli_agent_graph` 中组装：`SystemPrompt` / `UserInput` / `AgentState` 三个 `AnySource`  feeding `AgentLoopNode::create`，内部使用 `workflow::GraphBuilder::create_loop_decl`（见 `agent_framework/src/graph_executor/cli_agent_graph.cpp`）。

### 1.2 单次迭代体（顺序工具）

`agent_framework/src/node/agent_loop_node.cpp` 中，对本轮 `CallSpec` 列表使用普通 `for` 循环，每次 `toolbus->call_tool(c.name, c.arguments).get()`，将 `Message` 推入 `shared->state->history`。因此**同一 LLM 轮次内的多工具调用在实现上是串行的**，与文档中「可并行工具」的设计目标存在差距（需在 Roadmap 或实现中收敛）。

### 1.3 终止条件

`condition_func` 根据 `agent_config.max_iterations` 与 `shared->is_final` 返回是否退出循环；`exit_func` 写出 `final_answer`、`next_agent_state`、`llm_output` 等键（同文件）。

### 1.4 运行时完成度

`GraphExecutor::execute` 仍为 deferred future 内 `throw std::logic_error("... not implemented")`（`agent_framework/src/graph_executor/graph_executor.cpp`）。即：**图构建路径存在，统一执行管线尚未落地**。

## 2. humanus-cpp（HU）

### 2.1 主循环

`BaseAgent::run`：`state == RUNNING` 且 `current_step < max_steps` 时反复调用 `step()`；异常将 `state` 置 `ERR`（`humanus-cpp/agent/base.h`）。

### 2.2 ReAct 一步

`ToolCallAgent::think` 调用 `llm->ask_tool(..., available_tools.to_params(), tool_choice)`，解析 `tool_calls` 并写入 memory；`act` 对 `tool_calls` **顺序** `execute_tool`（`humanus-cpp/agent/toolcall.cpp`）。

### 2.3 计划流（外环）

`PlanningFlow::execute` 在创建初始计划后 `while (true)` 取当前步骤、按 `step_type` 选择 `executor`、执行 `_execute_step`；步骤间会 `reset` executor 记忆并注入摘要与用户续写提示（`humanus-cpp/flow/planning.cpp`）。这是 **HU 相对 AF 的一等「多步任务编排」**，与 AF 的「单 AgentLoop」不在同一抽象层。

## 3. Claude Code（CC）

### 3.1 Query 主循环

`claude-code-source-code/src/query.ts` 在 `buildQueryConfig()` 之后进入 `while (true)`；每轮处理 `messagesForQuery`、工具结果预算、compact、流式请求与 `runTools` 等。循环体体量大，体现 **产品与特性开关驱动** 的执行路径，而非库式最小内核。

### 3.2 工具并发策略

`services/tools/toolOrchestration.ts` 中 `runTools` 通过 `partitionToolCalls` 将工具调用分为：

- **可并发安全**的批次：多只读工具并行（`runToolsConcurrently`），再应用 `contextModifier`；
- **非并发安全**的批次：串行 `runToolsSerially`。

并发上限可由环境变量 `CLAUDE_CODE_MAX_TOOL_USE_CONCURRENCY` 控制（默认 10）。

## 4. 横向对照小结


| 项目     | AF               | HU                  | CC                           |
| ------ | ---------------- | ------------------- | ---------------------------- |
| 循环形态   | 声明式 Loop 节点      | 显式 `while` + `step` | 显式 `while` + 大量中间状态          |
| 同轮多工具  | 顺序               | 顺序                  | 分批并行/串行                      |
| 外层任务分解 | 需自建子图/多节点        | `PlanningFlow`      | 子 Agent、计划模式等（多模块）           |
| 异步模型   | C++ future / 调度器 | 同步阻塞式 httplib       | async/await + AsyncGenerator |


## 5. 对 AF 的执行层建议

- 在 **不破坏** `input_specs` 依赖推断的前提下，为 Tool 阶段增加 **可选并行策略**（元数据驱动），语义对齐 CC 的 partition。
- 优先 **落地** `GraphExecutor::execute`，使「模板 + 运行」闭环可测。

