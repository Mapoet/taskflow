
> **扩展阅读（三库）**：在 **humanus-cpp** 与 **agent_framework** 之外，若需对照 **Claude Code 客户端源码**（权限、上下文压缩、工具并发等），请参阅 [comparison_three_agents.md](./comparison_three_agents.md) 与分册 [deep_dive_execution.md](./deep_dive_execution.md)、[deep_dive_interfaces.md](./deep_dive_interfaces.md)、[deep_dive_security.md](./deep_dive_security.md)、[deep_dive_memory_context.md](./deep_dive_memory_context.md)；目录索引见 [README.md](./README.md)。

下面基于对 **`../humanus-cpp/`** 与当前仓库 **`agent_framework/`** 的源码与 README 的阅读，做技术状态对比、各自短板，以及对 **agent_framework** 的可改进方向（Ask 模式仅分析与建议，不改代码）。

---

## 1. 架构范式

| 维度 | humanus-cpp | agent_framework（taskflow） |
|------|-------------|-----------------------------|
| **执行模型** | 面向对象的 **同步 `while` 循环**：`BaseAgent::run` 里 `step()` 反复执行 | **声明式工作流**：`workflow::GraphBuilder`、`create_loop_decl`、节点间 **键值 I/O + 依赖自动推断** |
| **并行** | Agent 主循环单线程；`ToolCallAgent::act` 对 `tool_calls` **顺序 for 循环**执行 | 设计目标包含 **Taskflow 工作窃取**、工具侧 **`create_for_each` 并行调用** |
| **编排层次** | `BaseFlow` / `PlanningFlow`：多 Agent 映射、按 **计划步骤** 驱动执行 | `GraphExecutor`、`AgentLoopNode`：图级编排；**未发现**与 humanus 同类的「显式计划流」 |

humanus 的循环与工具执行是典型「单线程控制流」：

```103:110:/home/mapoet/Documents/CoWorks/humanus-cpp/agent/base.h
        while (current_step < max_steps && state == AgentState::RUNNING) {
            current_step++;
            logger->info("Executing step " + std::to_string(current_step) + "/" + std::to_string(max_steps));
            std::string step_result;

            try {
                step_result = step();
```

工具在 `act()` 里按顺序执行：

```76:83:/home/mapoet/Documents/CoWorks/humanus-cpp/agent/toolcall.cpp
    std::vector<ToolResult> results;

    std::string result_str;

    for (const auto& tool_call : tool_calls) {
        auto result = state == AgentState::RUNNING ? 
                    execute_tool(tool_call) : 
                    ToolError("Agent is not running, so no more tool calls will be executed.");
```

agent_framework 侧则把「Agent 循环」显式封装成可与整张图组合的节点（与 Taskflow/workflow 集成）：

```25:61:/home/mapoet/Documents/CoWorks/taskflow/agent_framework/include/node/agent_loop_node.hpp
/**
 * @brief Agent 循环节点封装类
 * 使用 create_loop_decl 将完整的 Agent Plan->Act->Observe->Reflect 循环封装为节点
 */
class AgentLoopNode {
public=
    ...
    static std::pair<std::shared_ptr<workflow::LoopNode>, tf::Task>
    create(
        workflow::GraphBuilder& builder,
        ...
```

---

## 2. LLM 与配置

- **humanus-cpp**：单一 **`LLM` 类 + httplib**，`ask` / `ask_tool`；**按名字单例** `get_instance`，配置来自 TOML 等。优点是上手快；缺点是 **多后端/多租户/测试替身** 不如接口化设计干净。
- **agent_framework**：**多适配器**（OpenAI / Anthropic / Gemini / vLLM 等）、`PromptRenderer`、与节点输入融合；更符合「生产多模型」路径，但 **概念与模块更多**，集成成本更高。

---

## 3. 工具与 MCP

- **humanus-cpp**：`BaseTool` / `ToolCollection`、**内置工具集**（如 README 提到的 filesystem、playwright、python_execute、planning 等），MCP 在 `BaseMCPTool` 里接 stdio/SSE；**ToolResult** 合并、`content_provider` 处理超长工具输出等 **产品化细节** 较多。
- **agent_framework**：**ToolBus**、schema 校验、MCP 客户端与本地工具统一路由；强调 **并行与安全边界**（规则里写明的模式）。**「开箱即用的 Browser / Python 沙箱级工具」** 在 humanus 示例里更突出，agent_framework 更偏 **框架能力 + 自己接工具**。

---

## 4. 记忆与向量检索

- **humanus-cpp**：**HNSW（hnswlib）**、嵌入模型抽象、`Memory` 带 **事实抽取、检索上限、与 LLM 联动更新** 等（`memory/base.h` 可见较多策略字段）。
- **agent_framework**：规划与文档侧重 **Faiss**、知识库节点、事件日志；向量路径不同，**未必更弱**，但 **与 mem0 式「会话记忆 + 事实管理」打包程度** 可能不如 humanus 单体仓库集中。

---

## 5. 多 Agent / 任务分解

- **humanus-cpp**：**`PlanningFlow`** — 用 `PlanningTool` + LLM 维护计划、按步骤选 executor、`_execute_step` 等，是 **高层任务编排**。
- **agent_framework**：有 **A2A**、子图/嵌套循环的 **架构文档**，但 repo 内 **没有** 与 humanus `PlanningFlow` 对等的「计划-分步-多 executor」一等公民模块（至少在 grep 层面无 planning 命名）。

---

## 6. 双方相对短板（互补视角）

**humanus-cpp 的短板（相对 agent_framework）**

1. **队内并行与调度**：无 Taskflow 级 **工作窃取**；多工具 **顺序执行**，吞吐在「多工具一轮」场景可能受限。  
2. **工作流可组合性**：依赖继承（`BaseAgent` → `ReActAgent` → `ToolCallAgent`），复杂 DAG、条件分支、与外部系统的 **声明式图** 不如 agent_framework。  
3. **LLM 层扩展性**：单例 + 单 client 形态，**多厂商/多协议** 的长期演进成本通常更高。  
4. **Flow 工厂**：`FlowType` 目前实质主要是 **PLANNING**（`flow_factory.h`），扩展更多 flow 时仍是同一套 OOP 堆叠。

**agent_framework 的短板（相对 humanus-cpp）**

1. **「应用层预设」**：humanus 把 **规划流、长工具结果分块、terminate、content_provider、浏览器/执行环境** 等绑得比较紧，agent_framework **更像底座**，同样能力需要更多拼接或示例。  
2. **循环内粘性检测**：humanus `BaseAgent` 有 `duplicate_threshold`、`is_stuck()` 等 **防死循环/重复** 思路；agent_framework 若要在图里统一做，需要明确落在节点策略或 guard 上（仓库里已有 `test_agent_loop_guard_*` 等测试信号，但整体产品叙事不如 humanus base 里集中）。  
3. **本地 tokenizer**：humanus 带 **BPE** 相关模块，便于 **本地粗算 token / 截断**；agent_framework 文档多依赖渲染与适配器侧统计，是否覆盖所有路径要按实现核对。  
4. **上手曲线**：workflow + 多模块，**简单 CLI agent** 的「最短代码路径」可能比 humanus `Chatbot` + `run()` 长。

---

## 7. 对 agent_framework 的改进建议（吸取 humanus 的长处）

下面只列 **方向**，不涉及改代码。

1. **增加可选的 Planning / 多步编排层**  
   - 类似 `PlanningFlow`：**显式计划结构**（步骤状态、完成标记、executor 选择），可先用 **子图 + 状态 Source** 实现，再考虑是否与 A2A「任务」模型对齐。  
   - 与现有 **`AgentLoopNode`** 的关系：Planning 作为 **外环**，AgentLoop 作为 **每步的内环**。

2. **「高价值默认工具包」或官方示例对齐**  
   - 把 humanus 证明有用的模式 **文档化/示例化**：超长工具输出 **分块与索引读取**（content_provider 模式）、**显式 terminate**、与 MCP 混排的 **特殊工具** 语义。

3. **并行策略可配置**  
   - 框架已具备并行能力时，在 **同一轮 tool_calls** 上提供清晰策略：**独立只读工具并行**、**有副作用顺序化**，并在文档中写明与人类可观测顺序的差异。

4. **粘性检测与预算标准化**  
   - 把 **重复输出检测、最大步数、token/时间预算** 提升为 **AgentConfig / 循环 guard** 的一等配置，与现有测试（如 loop guard）统一到用户可见 API。

5. **简化入口（不改变内核）**  
   - 保留图引擎的同时，提供 **薄封装**：「只想 ReAct + ToolBus + Memory」时几行搭出与 humanus `ToolCallAgent` 等价的运行时，降低从 humanus 迁来的成本。

6. **记忆策略可选包**  
   - 若目标场景包含 mem0 式 **事实抽取 + 向量回忆**，可增加可选模块或插件接口，与现有 **MemoryStore + VectorStore** 组合，避免每个应用自己抄一套 humanus `Memory` 逻辑。

---

## 8. 简要结论

- **humanus-cpp** 更强在：**轻量、继承式 ReAct/ToolCall、计划流、丰富的单体内置工具与记忆技巧、HNSW+MCP 一体化**；弱在 **并行与工作流组合度、多 LLM 适配的工程化分层**。  
- **agent_framework** 更强在：**声明式图、Taskflow 调度、ToolBus+MCP+多模型+多端+A2A 的平台化**；弱在 **开箱「完整 Manus 式」应用路径、显式规划层、以及部分人性化运维细节（粘性、分块）需自己补**。  

若你后续希望把某一条改进落到具体设计（例如 Planning 子图与 `AgentLoopNode` 的接口草图），可以在 Agent 模式下再拆任务实现。