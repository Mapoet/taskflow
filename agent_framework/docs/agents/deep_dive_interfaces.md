# 深度分册：接口与可扩展性

## 1. agent_framework

### 1.1 工具抽象

- **`ToolInterface`**：`call(name, args) -> future<json>`、`get_tool_meta`、`validate_arguments` 等（`agent_framework/include/agent/toolbus.hpp`）。
- **`LocalTool`**：包装 `std::function<json(const json&)>`，便于单元测试与轻量扩展。
- **`ToolBus`**：注册本地工具、加载 MCP 服务、导出 `export_as_llm_tools()`；调用路径上带 **JSON Schema 校验**（`call_tool` 内）。

### 1.2 LLM 与提示

- **`LLMClient`** + **`PromptRenderer`**：与节点输入合并（`LLMNode`），支持多提供商适配器（见各 `llm_client` 实现与 `types.hpp` 中 `LLMInput` / `LLMOutput`）。

### 1.3 图与节点

- **`AgentLoopNode::create`**：将整段 ReAct 循环封装为 **单个 Loop 节点**（`agent_framework/include/node/agent_loop_node.hpp`）。
- **`NodeFactory`**：工厂方法创建 LLM、知识库 Source、工具聚合等（`agent_framework/include/node/node_factory.hpp`），降低直接操作 `GraphBuilder` 的样板代码。

### 1.4 Skills（WP1.8）

- **`SkillRegistry` / `SkillLoader` / `SkillServices`**：技能发现与 frontmatter；**`register_skill_script_tool`** 向 `ToolBus` 注册 `run_skill_script`（见 `agent_framework/src/skills/skill_script_tool.cpp` 与 `graph_executor/cli_agent_graph.cpp` 中 `deps.skills` 分支）。

### 1.5 扩展方式评价

- **优点**：新增能力优先 **组合节点** 与 **注册工具**，符合「声明式数据流」规则；测试可替换 `LLMClient` / 局部工具。
- **缺口**：自定义工作流若不走 `build_cli_agent_graph`，仍依赖调用方直接使用 `GraphBuilder`；`build_custom_workflow` 未实现。

## 2. humanus-cpp

### 2.1 继承树

- **`BaseAgent`**：记忆、`run`/`step`、粘性检测 `is_stuck`（`humanus-cpp/agent/base.h`）。
- **`ReActAgent`**：抽象 `think`/`act`（`humanus-cpp/agent/react.h`）。
- **`ToolCallAgent`**：具体 `think`/`act` + `ToolCollection` + `tool_choice`（`humanus-cpp/agent/toolcall.h`）。
- **`Humanus`**：预置工具列表构造函数（`humanus-cpp/agent/humanus.h`）。

### 2.2 工具集合

- **`ToolCollection`**：名称到 `shared_ptr<BaseTool>` 的映射，`to_params()` 供 LLM（`humanus-cpp/tool/tool_collection.h`）。
- **内置工具**：如 `Filesystem`、`PythonExecute`（MCP 包装）、`Playwright` 等，通过 **子类化 + 注册表** 扩展。

### 2.3 LLM 访问

- **`LLM::get_instance(config_name)`** 单例字典（`humanus-cpp/include/llm.h`）；适合快速集成，**多实例隔离**需额外设计。

### 2.4 Flow

- **`BaseFlow`**：`agents` map + `primary_agent_key`（`humanus-cpp/flow/base.h`）。
- **`PlanningFlow`**：与 `PlanningTool` 紧耦合（`flow/planning.cpp`）。

### 2.5 扩展方式评价

- **优点**：新 Agent 类型 **继承 + 组合工具** 即可；示例路径短。
- **缺点**：复杂分支/并行子图需手写 C++ 控制流，**不如 AF 图组合清晰**。

## 3. Claude Code

### 3.1 Tool 类型

- **`Tool.ts`**：集中定义工具类型、`ToolUseContext`、`inputSchema`（zod）、进度类型、权限相关字段（如 `ContentReplacementState`）等，是 **整个产品的工具契约中心**。

### 3.2 发现与路由

- **`findToolByName`**（同文件导出）：在 `toolUseContext.options.tools` 中解析工具，供 orchestration 与权限层使用。

### 3.3 权限回调类型

- **`CanUseToolFn`**（`claude-code-source-code/src/hooks/useCanUseTool.tsx`）：`(tool, input, toolUseContext, assistantMessage, toolUseID, forceDecision?) => Promise<PermissionDecision>`。虽挂在 React hook 文件，**类型即产品扩展点**：允许/拒绝/改写输入。

### 3.4 扩展方式评价

- **优点**：工具、权限、遥测、MCP 在同一进程内 **类型化衔接**；新工具多为 **独立目录 + 注册**。
- **缺点**：依赖 Bun/Node 生态与 **AppState**，难以作为 **无头 C++ 库** 复用。

## 4. 接口策略对照

| 能力 | AF | HU | CC |
|------|----|----|-----|
| 工具注册 | 总线 + 接口 | 集合 map | 工具数组 + 名称查找 |
| 参数校验 | JSON Schema | LLM/工具侧约定 | zod `safeParse` + schema |
| 组合新行为 | 新节点/子图 | 新子类/新 Flow | 新 Tool 模块 + feature 开关 |
| 权限钩子 | 无一等公民（仅 allowlist） | 弱（工具内） | `CanUseToolFn` + 规则引擎 |

## 5. 对 AF 的接口建议

- 增加 **`std::function` 可选 pre-hook**：`call_tool` 前 `(name, json&) -> expected<bool, error>`，语义对齐 CC 的 permission allow/deny/modify input。
- **`ToolMeta` 扩展字段**：如 `concurrency_safe: bool`、`risk_tier`，供未来并行调度与策略使用。
- 保持 **`ToolInterface` 稳定**，MCP/HTTP 工具与本地工具继续走同一出口，避免双轨 API。
