# Agent Framework 节点封装模块

## 概述

`node/` 目录提供了将 Agent Framework 各个功能模块封装为 workflow 节点的便捷接口。这些节点可以直接在 `workflow::GraphBuilder` 中使用，简化工作流的构建。

## 目录结构

```
node/
├── node_factory.hpp      # 节点工厂类（提供统一的节点创建接口）
├── llm_node.hpp          # LLM 节点封装
├── knowledge_base_node.hpp # 知识库 Source 节点封装
├── tool_call_node.hpp    # 工具调用节点封装
├── agent_loop_node.hpp   # Agent 循环节点封装
├── ui_sink_node.hpp      # UI 输出 Sink 节点封装
├── nodes.hpp             # 统一包含文件
└── README.md             # 本文件
```

## 核心设计理念

1. **封装 Agent 功能为节点**：将 LLM 调用、工具调用、知识库检索等功能封装为 workflow 节点
2. **声明式 API**：使用 workflow 的声明式 API，自动推断依赖关系
3. **统一接口**：所有节点创建都遵循相同的接口模式
4. **可组合性**：节点可以组合成复杂的工作流

## 使用示例

### 1. 使用节点工厂创建节点

```cpp
#include "node/nodes.hpp"

// 创建 LLM 节点
auto [llm_node, llm_task] = agent_framework::node::NodeFactory::create_llm_node(
    builder,
    "LLM",
    llm_client,
    {
        {"SystemPrompt", "prompt"},
        {"UserInput", "query"},
        {"KnowledgeBase", "context"}
    },
    [](std::string_view token) {
        std::cout << token << std::flush;
    }
);

// 创建知识库 Source 节点
auto [kb_source, kb_task] = agent_framework::node::NodeFactory::create_knowledge_base_source(
    builder,
    "KnowledgeBase",
    vector_store,
    encoder_manager
);

// 创建工具调用节点
auto [tool_node, tool_task] = agent_framework::node::NodeFactory::create_tool_call_node(
    builder,
    "ToolCall",
    toolbus,
    {{"LLM", "tool_calls"}}
);
```

### 2. 直接使用节点类

```cpp
#include "node/llm_node.hpp"

// 直接使用 LLMNode 类
auto [llm_node, llm_task] = agent_framework::node::LLMNode::create(
    builder,
    "LLM",
    llm_client,
    {
        {"SystemPrompt", "prompt"},
        {"UserInput", "query"}
    }
);
```

### 3. 创建完整的 Agent 循环节点

```cpp
#include "node/agent_loop_node.hpp"

AgentConfig config;
config.name = "MyAgent";
config.system_prompt = "You are a helpful assistant.";
config.max_iterations = 10;

auto [agent_node, agent_task] = agent_framework::node::AgentLoopNode::create(
    builder,
    "AgentLoop",
    config,
    llm_client,
    toolbus,
    memory_store,
    vector_store,
    {{"UserInput", "query"}},
    {"final_answer", "reasoning"}
);
```

## 节点类型

### Source 节点

- **KnowledgeBaseSourceNode**：知识库查询 Source 节点
  - 输入：查询文本（通过 `set_query()` 方法设置）
  - 输出：`context`（上下文摘要）、`results`（检索结果）、`citations`（引用信息）

### Process 节点

- **LLMNode**：LLM 调用节点
  - 输入：`system_prompt`, `user_prompt`, `context`, `history`, `tools`, `image_data`, `audio_data`
  - 输出：`tool_calls`, `reasoning`, `is_final`, `final_answer`, `audio_out`

- **ToolCallNode**：工具调用节点
  - 输入：`call_spec`（单个工具调用）或 `call_list`（工具调用列表）
  - 输出：`result`（单个结果）或 `results`（并行结果列表）

- **AgentLoopNode**：Agent 循环节点（封装完整的 Plan->Act->Observe->Reflect 循环）
  - 输入：`query`, `prompt`
  - 输出：`final_answer`, `reasoning`, `tool_calls`

### Sink 节点

- **UISinkNode**：UI 输出节点
  - **CLI**：命令行输出
  - **ImGui**：ImGui 界面输出
  - **Web**：SSE/WebSocket 输出

- **VectorStoreSink**：向量存储 Sink 节点
  - 输入：`embedding`, `metadata`, `content`

## 实现说明

所有节点封装都遵循以下原则：

1. **静态工厂方法**：使用静态 `create()` 方法创建节点
2. **自动依赖推断**：通过 `input_specs` 自动建立依赖关系
3. **类型安全**：使用 `std::any_cast` 进行运行时类型转换
4. **错误处理**：节点执行失败时返回错误信息

## 扩展节点

要添加新的节点封装，请遵循以下步骤：

1. 在 `node/` 目录下创建新的头文件（如 `my_node.hpp`）
2. 定义节点封装类，提供静态 `create()` 方法
3. 在 `nodes.hpp` 中包含新头文件
4. 在 `node_factory.hpp` 中添加工厂方法（可选）

## 参考

- `workflow/include/workflow/nodeflow.hpp`：workflow 节点基础 API
- `readme/guide_agent.md`：Agent Framework 设计文档
- `agent_framework/docs/architecture/overview.md`：架构设计文档

