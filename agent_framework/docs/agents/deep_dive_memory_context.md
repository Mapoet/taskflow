# 深度分册：记忆、上下文层级与观测

下文将「层级」拆为：**存储层**（消息存在哪）、**策略层**（截断/压缩/检索）、**观测层**（如何调试与度量）。

## 1. agent_framework

### 1.1 存储层

- `**internal::AgentThreadState`**（`agent_framework/include/agent/internal/agent_thread_state.hpp`）：
  - `history`：`std::vector<Message>`，轮次间累积 user/assistant/tool 等；
  - `iteration`：已完成 LLM 调用次数；
  - `initial_user_prompt`、**Skills** 相关 `skill_prompt_cache` / `active_skill_id`。
- `**Message`**（`agent_framework/include/agent/types.hpp`）：`role`、`content`、`tool_call_id`、`tool_name`、`tool_result`、`timestamp` — **扁平消息列表**，无 CC 式的 block 数组。

### 1.2 策略层

- **循环退出**：`max_iterations`、`is_final`、无工具调用时的终局判定（见 `agent_loop_node.cpp`）。
- **Guard**：同轮重复相同工具+参数可触发提前终局（环境变量 `AGENT_LOOP_GUARD_*`）。
- **知识库**：可通过 `**NodeFactory` / 知识库节点** 与向量库组合，将检索结果注入 LLM 输入（与 `AgentThreadState` 正交，属 **图级** 扩展）。
- **长工具结果**：框架核心 **未** 内置 HU 式 `content_provider` 分块；需应用层工具返回引用或自行截断。

### 1.3 观测层

- `AGENT_LOG_LEVEL`、`AGENT_TEST_AGENT_LOOP_DEBUG` 等控制 `agent_loop_node.cpp` 中的 clog/stdout 轨迹。
- 无内置 transcript 文件格式或遥测管道（**推断**：由集成方负责）。

## 2. humanus-cpp

### 2.1 存储层

- `**BaseMemory`**：`std::deque<Message> messages`（`humanus-cpp/memory/base.h`）。
- `**Memory**`：在 `add_message` 时维护 `**num_tokens_messages**`，超出 `max_messages` / `max_tokens_messages` 时将旧消息 **挤出到向量存储**（若 `retrieval_enabled`）。

### 2.2 策略层

- `**get_messages(query)`**：若启用检索且 `query` 非空，先 **embedding 搜索** `vector_store`，按 `max_tokens_context` 将 `<memory>...</memory>` 形式 **前置注入**，再拼接当前 deque（同文件 `get_messages` 实现）。
- **超长工具输出**：`ToolCallAgent::act` 中超过阈值时用 `**content_provider->handle_write`** 分块，并改写 tool 消息内容为「请用 content_provider 读取块」（`humanus-cpp/agent/toolcall.cpp`）。
- **事实抽取**：`FactExtract`、`MemoryTool` 等与 LLM 联动（`Memory` 构造函数中初始化）。

### 2.3 观测层

- **spdlog**：步骤与工具结果日志（含 emoji 标记便于人工扫日志）。
- **Tokenizer 模块**：仓库含 BPE 等，可用于 **本地粗算 token**（与 AF 依赖适配器统计的路径不同）。

## 3. Claude Code

### 3.1 存储层

- **会话消息链**：`Message` 类型包含多种块（`tool_use`、`tool_result`、附件等），存于应用状态；与 Anthropic API 形状对齐（见 `types/message` 与 `normalizeMessagesForAPI` 等）。

### 3.2 策略层

- **工具结果预算**：`applyToolResultBudget` 在 query 循环早期对 `messagesForQuery` 处理，并可 **持久化 replacement**（`query.ts` 中与 `contentReplacementState`、`recordContentReplacement` 配合）。
- **外置存储**：`toolResultStorage.ts` 将大体积结果落盘并按 `tool_use_id` 索引，减轻 prompt 体积（详见该文件注释与导出函数）。
- **压缩**：`services/compact/autoCompact.ts` 等根据 **有效上下文窗口**、失败熔断、`compactConversation` 等维护 **autoCompactTracking**；与 **token 估算**（`utils/tokens.ts`）联动。
- **Skill 预取**：`query.ts` 中 `skillPrefetch?.startSkillDiscoveryPrefetch`（特性开关），与消息、工具上下文并行。

### 3.3 观测层

- **遥测**：`logEvent`、session tracing、权限决策日志等（分散于 `utils/telemetry`、`permissionLogging` 等）。
- **Profiler**：`queryProfiler`、`headlessProfiler` 等。

## 4. 三库对照表


| 维度         | AF                          | HU                       | CC                       |
| ---------- | --------------------------- | ------------------------ | ------------------------ |
| **主对话载体**  | `AgentThreadState::history` | `Memory::messages` deque | 富结构 Message 列表           |
| **挤出+回忆**  | 可选 KB 节点；非内建                | 向量检索 + token 预算注入        | compact + tool result 外置 |
| **超长工具结果** | 应用负责                        | content_provider 分块      | 预算 + 磁盘替换                |
| **上下文窗口**  | 依赖 LLM 适配器                  | `max_tokens_`* 配置        | autoCompact + 环境变量调窗口    |
| **观测**     | 环境变量日志                      | 日志 + tokenizer           | 遥测 + profiler            |


## 5. 对 agent_framework 的建议

1. **可选 Memory 策略模块**：提供与 HU 类似的 **token 计数 + 挤出队列** 接口，但保持 **默认实现简单**，避免强迫所有用户启用向量。
2. **工具结果策略**：在 `ToolBus` 或 Loop 后处理增加 **可选「大于 N 字符则写临时文件 + 返回路径」** 钩子，对齐 CC/HU 的工程经验。
3. **调试包**：定义稳定的 **JSON trace 事件**（iteration、tool name、latency、error code），便于与 AF 的「无 UI」部署集成。

