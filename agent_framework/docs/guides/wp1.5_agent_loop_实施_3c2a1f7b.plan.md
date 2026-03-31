---
name: WP1.5：Agent 循环（workflow）— 实施计划
overview: 基于 phase-1-plan.md 与 phase-1-wp5.md，锁定 create_loop_decl 子图式 Agent 循环（LLM→工具→history合并→条件→下一轮），定义稳定的 loop I/O keys、状态桶（history/iteration）、顺序工具执行聚合策略，并补齐可重复的 mock 集成测试与图 dump 验收。
todos:
  - id: wp15-io-keys
    content: 定义并锁定 loop I/O keys（字符串常量），统一 body 输出键与 condition 读键
    status: pending
  - id: wp15-state
    content: 定义 AgentThreadState（history/iteration/last_error/initial_user_prompt 可选）与在 workflow key-based I/O 中的传递方式
    status: pending
  - id: wp15-render-llm-node
    content: 实现 LLMNode 输入提取与 PromptRenderer 集成（render→invoke_with_rendered 或 invoke），输出标准化 LLMOutput
    status: pending
  - id: wp15-tool-aggregator
    content: 实现顺序工具执行聚合节点（ToolAggregator）：执行 LLMOutput.tool_calls，产出 tool Messages 与更新后的 history 增量
    status: pending
  - id: wp15-state-merge
    content: 实现 StateMerge 节点：合并 assistant/tool 消息到 state.history，递增 iteration，输出下一轮所需键
    status: pending
  - id: wp15-condition-exit
    content: 实现 check_loop_condition 与 exit_handler：max_iterations/has_tool_calls/is_final 规则与最终输出映射
    status: pending
  - id: wp15-agent-templates
    content: 实现/整理 agent_templates.build_cli_agent_graph，把 AgentLoopNode 封装成可注入 LLMClient/ToolBus/PromptRenderer 的工厂
    status: pending
  - id: wp15-tests
    content: 新增 WP1.5 mock 集成测试：2轮 tool + 1轮 final，断言 tool 执行次数、iteration、final_answer，并可 dump 图
    status: pending
isProject: false
---

# WP1.5：Agent 循环（workflow）— 实施计划（详实可执行）

## 0. 目标与范围（对齐 phase-1-plan.md §3 WP1.5 / phase-1-wp5.md）

- **目标**：用 `workflow::GraphBuilder::create_loop_decl` 构建单图闭环：
  - `LLM` → （若有 tool_calls）→ **顺序执行工具** → **合并 history** → `condition_func` → 下一轮 `LLM`
  - 直到：tool_calls 为空且 is_final/final_answer 满足退出，或达到 `AgentConfig.max_iterations`。
- **非目标**：并行工具默认不跑；KnowledgeBase/VectorStore 可为 nullptr/关闭；取消传播与 UI sink 属 WP1.6+。

## 1. 现状差距（基于仓库当前代码）

- `agent_framework/src/node/agent_loop_node.cpp` 当前是**占位/简化**：
  - condition_func 未实现 iteration（注释写明限制）。
  - loop body 目前直接调用 `ToolCallNode::create_parallel`，而 WP1.5 锁定首版应为**顺序执行**（便于日志与可控）。
  - 未实现把 tool 结果回灌到 history 并驱动下一轮 LLM 的状态归约。
- `LLMNode/ToolCallNode` 头文件接口已存在，但需要把它们组合进 loop 子图，并定义稳定 keys。

## 2. 锁定决策（避免歧义）

### 2.1 iteration 语义（锁定 A）
- `iteration` = 已完成的 **LLM 调用次数**。
- 每完成一轮 loop body（包含一次 LLM 调用）后 `++iteration`。

### 2.2 工具执行策略（锁定：顺序）
- 首版：按 `LLMOutput.tool_calls` 顺序逐个 `ToolBus::call_tool(...).get()`。
- 若超过 `AgentConfig.max_tool_calls_per_iteration`：**截断**后执行（并记录日志）。

### 2.3 PromptRenderer 位置（锁定：在 LLMNode 内集成）
- `LLMNode` 负责：
  - 组装 `LLMInput`（system/user/history/tools/context/image/audio/extra_variables）
  - 调用 `PromptRenderer::render(...)` 得到 `RenderedPrompt`
  - 调 `LLMClient::invoke_with_rendered(...)`（或等价 API；若当前 LLMClient 只有 `invoke(LLMInput)`，则先锁定一种调用路径并文档化）

## 3. Loop I/O keys（wp15-io-keys）

新增一个仅含常量的头文件（建议放 internal）：
- `agent_framework/include/agent/internal/loop_io_keys.hpp`

建议键集合（锁定名称，避免 any_cast 错配）：
- **输入键**：
  - `kUserQuery`：string（首轮用户输入）
  - `kSystemPrompt`：string（可选，来自 AgentConfig 或外部）
  - `kAgentState`：`std::shared_ptr<AgentThreadState>`
- **LLM 输出键**：
  - `kLlmOutput`：`LLMOutput`
- **工具执行输出键**：
  - `kToolMessages`：`std::vector<Message>`（本轮工具结果消息）
  - `kToolHadError`：bool（是否有工具错误；首版可不退出但写入 last_error）
- **归约输出键**：
  - `kNextAgentState`：`std::shared_ptr<AgentThreadState>`
  - `kIsFinal`：bool
  - `kFinalAnswer`：string

> 原则：body_builder_fn 的输出 keys 必须包含 condition_func 读取的 keys。

## 4. 状态桶 AgentThreadState（wp15-state）

在 `agent_framework/include/agent/internal/agent_thread_state.hpp` 定义：

```cpp
struct AgentThreadState {
  std::vector<Message> history;
  int iteration = 0;
  std::string last_error;
  std::string initial_user_prompt; // 可选：防截断丢意图
};
```

传递方式（锁定：通过 key-based I/O 传 shared_ptr）：
- Loop 外部先创建 state（Source 节点输出 `kAgentState`）
- 每轮 StateMerge 产出 `kNextAgentState`，并作为下一轮 LLMNode 的 input_specs 依赖

## 5. Loop body 子图结构（核心）

### 5.1 body_builder_fn 节点拓扑（概念）

```mermaid
flowchart TD
  AgentStateSrc[AgentState(kAgentState)] --> LLMNode
  UserQuerySrc[UserQuery(kUserQuery)] --> LLMNode
  SystemPromptSrc[SystemPrompt(kSystemPrompt)] --> LLMNode
  ToolListSrc[ToolList(tools)] --> LLMNode

  LLMNode[LLMNode -> kLlmOutput] --> ToolAgg
  AgentStateSrc --> ToolAgg

  ToolAgg[ToolAggregator -> kToolMessages] --> StateMerge
  LLMNode --> StateMerge
  AgentStateSrc --> StateMerge

  StateMerge[StateMerge -> kNextAgentState,kIsFinal,kFinalAnswer] --> condFunc
```

### 5.2 LLMNode（wp15-render-llm-node）
文件：
- `agent_framework/src/node/llm_node.cpp`
- `agent_framework/include/node/llm_node.hpp`（如需补参数：PromptRenderer 注入）

实现要点：
- `extract_llm_input(inputs)`：
  - `system_prompt`：来自 `kSystemPrompt`（若无则空）或 `AgentConfig.system_prompt`
  - `user_prompt`：首轮来自 `kUserQuery`；后续仍保留 `state.initial_user_prompt`（建议）或使用同一 query
  - `history`：来自 `AgentThreadState.history`
  - `tools`：每轮从 `toolbus->export_as_llm_tools()`（可在 ToolList Source 节点提前输出）
  - `extra_variables`：沿用 input.extra_variables（若 CLI 支持变量参数，在外部填入）
- 先 `renderer->render(llm_input, model_name)`，再调用 LLMClient，产出 `LLMOutput`。
- 输出键：至少 `kLlmOutput`。

### 5.3 ToolAggregator（wp15-tool-aggregator）
