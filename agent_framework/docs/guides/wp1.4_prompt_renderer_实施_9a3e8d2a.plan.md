---
name: WP1.4 PromptRenderer 实现计划
overview: 基于 phase-1-plan.md（D5）与 phase-1-wp4.md 的锁定约束，将 PromptRenderer（LLMInput→RenderedPrompt）的接口契约、历史截断、tools_json 供应商形态、context/多模态注入与测试矩阵细化为可直接落地的任务清单与 DoD。
todos:
  - id: wp14-types-contract
    content: 锁定 types.hpp / prompt_renderer.hpp 的契约与最小改动面（history/tool 对齐字段、system 策略、当前轮 user 规则）
    status: pending
  - id: wp14-history-truncate
    content: 实现 HistoryFormatter::truncate 成对截断规则（tool_calls 与 tool 结果组）并补单测
    status: pending
  - id: wp14-tool-formatters
    content: 实现 ToolFormatter（OpenAI/Anthropic）tools_json 形态，确保与 WP1.1 request builder 兼容；未命中模型回退策略
    status: pending
  - id: wp14-render-core
    content: 实现 PromptRenderer::render 的组装顺序（system+history+user），context 注入策略、rendered_text 稳定输出、token 粗估
    status: pending
  - id: wp14-multimodal
    content: 实现/固化多模态注入（至少 image）为 OpenAI content parts；保持纯文本路径不变
    status: pending
  - id: wp14-tests-cmake
    content: 新增 WP1.4 单测（fixture/快照）并接入 CMake/CTest，覆盖空 history、多轮 tool、截断边界、tools_json 两供应商
    status: pending
isProject: false
---

# WP1.4：PromptRenderer — 实现计划（详实可执行）

## 0. 目标、边界、依赖

### 0.1 目标（对齐 phase-1-plan.md D5 与 phase-1-wp4.md §1）
- **G1**：提供稳定的 `PromptRenderer::render(LLMInput, model_name) -> RenderedPrompt`，输出字段 **确定性强**，便于 WP1.1 适配器直接消费。
- **G2**：支持历史截断：可配置 `max_history_messages`（首版按**条数**裁剪），并保证**不会产生孤儿 tool 消息**。
- **G3**：生成供应商兼容的 `tools_json`：OpenAI 与 Anthropic 两种形态均可由 `ToolFormatter` 产出。
- **G4**：`LLMInput.context` 非空时按固定策略稳定注入（择一写死并测试）。
- **G5**：多模态占位：至少支持 `image_data` 注入到 user message 的 OpenAI content parts。
- **G6**：单测以 fixture/快照方式验证 `messages/tools_json`，避免随机字段干扰。

### 0.2 非目标（与 phase-1-wp4.md §1.2 一致）
- 精确 tokenizer；`total_tokens` 可用粗估。
- 历史摘要压缩与 RAG（阶段 3）。
- Gemini 完整工具格式（可维持 stub/最小实现）。

### 0.3 依赖与接口边界
- **上游输入**：`LLMInput`（`types.hpp`）。
- **输出**：`RenderedPrompt`（`types.hpp`），被 WP1.1 的 OpenAI/Anthropic adapter 构建请求时消费。
- **工具来源**：`ToolBus::export_as_llm_tools()` 输出的 `std::vector<ToolMeta>`。
- **循环写 history**：由 WP1.5 负责把 tool_calls/tool_result 追加进 `LLMInput.history`；WP1.4 只负责格式化与截断，**不得**调用 LLM 或 ToolBus。

## 1. 锁定决策（避免实现歧义）

### 1.1 `RenderedPrompt.messages` 的统一形态
- **锁定**：`RenderedPrompt.messages` 统一为 **OpenAI Chat messages 数组**（`vector<json>`），其中包含 `role` 与 `content`：
  - `system`：仅由 `LLMInput.system_prompt`（+ context 注入策略）生成，**不从 history 里取**。
  - `user/assistant/tool`：来自 `LLMInput.history` 的 `Message` 转换。
  - **最后一条**一定追加当前轮 `{role:"user", content:user_prompt(±context)}`（见 1.3）。

### 1.2 context 注入策略（固定策略，择一写死）
- **锁定**：采用 **策略 A（system 追加）**：
  - `system_content = system_prompt + \"\\n\\n## Retrieved context\\n\" + context`（仅当 context 非空）。
  - `user_prompt` 不再被 context 前缀污染（避免对问题语义的意外变形）。

> 注：当前 `prompt_renderer.cpp` 里是把 context 拼到 user（相当于策略 B）。WP1.4 应按本锁定决策改回策略 A，并更新单测。

### 1.3 当前轮 user 与 history 的关系
- **锁定**：`LLMInput.user_prompt` **总是**作为本轮最后一条 user message；`LLMInput.history` 不应包含“当前用户这句”（由 WP1.5 维护）。
- 若外部错误地把 user 放入 history，WP1.4 不做去重（首版保持简单），但在测试/文档中声明该约定。

### 1.4 tools_json 形态（对齐 phase-1-wp4.md §3.2）
- OpenAI：`tools_json` 为数组 `[{type:\"function\", function:{name,description,parameters}}]`
- Anthropic：`tools_json` 为数组 `[{name,description,input_schema}]`
- `ToolFormatter` 的 `format_tools()` **直接**产出上述结构；适配器只做字段放置（不再猜）。

### 1.5 历史截断“成对约束”
- **锁定**：按 `max_history_messages` 从旧到新删除，且若切断点落在 tool 相关组中，必须整组删除，避免孤儿：
  - 典型组：`assistant(tool_calls)` + 若干 `tool(...)` + `assistant`（后续）。
  - 在现有 `Message` 结构缺少 tool_calls 字段时，首版以 `role/tool_name/tool_result` 的组合启发式识别组边界（见 2.2 与 3.1）。

## 2. 代码与类型改动清单（按仓库现状落地）

### 2.1 现状核对（来自现有代码）
- `include/agent/prompt_renderer/prompt_renderer.hpp` 已声明：
  - `PromptTemplate` / `ToolFormatter` / `HistoryFormatter` / `PromptRenderer`。
- `src/prompt_renderer/prompt_renderer.cpp` **已实现大部分逻辑**（模板/工具 formatter/部分 history formatter/多模态注入/渲染主流程）。
- 但 `src/prompt_renderer/history_formatter.cpp`、`tool_formatter.cpp`、`prompt_template.cpp` 仍是 TODO 空壳，与 `prompt_renderer.cpp` 存在实现重复/错位（需要收敛）。

### 2.2 types.hpp 的最小必要补齐（是否要改类型）
阶段 1 推荐最小侵入：
- **不强制**给 `Message` 增加 `tool_call_id` / `tool_calls`（因为 `CallSpec` 已有 `tool_call_id`，而 WP1.5 也可在历史里用 `Message.content` 承载 assistant/tool 的 JSON 串）。
- 但为了真正做到“成对截断”，建议在 WP1.4 内部定义一个**组识别规则**：
  - `role==\"assistant\"` 且 `content` 可 parse 且含 `tool_calls`（OpenAI 形态）→ 视为 tool_calls 起点
  - `role==\"tool\"` → 视为 tool_result
  - 否则普通消息

若你希望更强类型安全，可在同 PR 增加：
- `Message::tool_call_id`（optional string）
- `Message::tool_calls`（optional json）
并同步 WP1.5 写 history 的逻辑。但此属于“跨 WP”改动，需在本计划中单列并与 WP1.5 一起验收。

## 3. 任务拆分（实现顺序与文件落点）

### T0（wp14-types-contract）：契约锁定与代码结构收敛
- **目标**：避免 `prompt_renderer.cpp` 与 `*_formatter.cpp` 空壳重复；明确“谁实现在哪里”。\n- **动作**：\n  - 将真实实现移动/拆分到：\n    - `src/prompt_renderer/prompt_template.cpp`\n    - `src/prompt_renderer/tool_formatter.cpp`\n    - `src/prompt_renderer/history_formatter.cpp`\n    - `src/prompt_renderer/prompt_renderer.cpp` 仅保留 PromptRenderer 核心编排（或保留现状但确保另外三个文件不再是 TODO 空壳）。\n- **输出**：编译不重复定义，链接目标保持稳定。\n\n### T1（wp14-history-truncate）：历史截断 + 成对约束\n- **实现位置**：`OpenAIHistoryFormatter::truncate`（建议放在 `history_formatter.cpp`）\n- **规则**（严格按 phase-1-wp4.md §4.2）：\n  - 先按条数裁剪最旧消息\n  - 若裁剪后序列开头处于 tool 组中间：继续向前删除直到落在“完整边界”（例如以 `role==\"user\"` 作为安全边界，或直到不再出现 `role==\"tool\"` 起始）\n- **并行实现**：`format_as_messages` 需要能正确格式化 tool message（至少 `role/tool` 的 `name/content`），为 WP1.1 request builder 提供确定性。\n\n### T2（wp14-tool-formatters）：tools_json 两供应商形态\n- **实现位置**：`tool_formatter.cpp`\n- **要求**：\n  - `OpenAIToolFormatter::format_tools` 返回 `[{type:\"function\", function:{...}}]`\n  - `AnthropicToolFormatter::format_tools` 返回 `[{name,description,input_schema}]`\n  - `supported_models()` 返回 `gpt-*`/`claude-*` 等 pattern；`PromptRenderer::get_tool_formatter` 未命中时回退 OpenAI formatter 并写日志。\n\n### T3（wp14-render-core）：PromptRenderer::render 核心编排\n- **实现位置**：`prompt_renderer.cpp`\n- **组装顺序（锁定）**：\n  1. 生成 system message（按策略 A：system 附加 context）\n  2. `history' = truncate(history, max_history_messages)`\n  3. `messages = [system] + format_as_messages(history') + [{role:\"user\", content:user_prompt}]`\n  4. `tools_json = formatter->format_tools(input.tools)`（formatter 按 model_name 选择）\n  5. `rendered_text`：用于调试，建议 = system + `format_as_text(history')` + user\n  6. `total_tokens`：字符粗估\n\n### T4（wp14-multimodal）：多模态注入（image 优先）\n- **实现位置**：`PromptRenderer::integrate_multimodal_input`\n- **锁定**：仅修改最后一条 user message，把 string content 替换为 OpenAI content parts：\n  - `[{type:\"text\", text:\"...\"}, {type:\"image_url\", image_url:{url:\"data:image/jpeg;base64,...\"}}]`\n\n### T5（wp14-tests-cmake）：单测与 CTest 接入\n- **新增测试**：`agent_framework/tests/test_prompt_renderer_wp4.cpp`\n- **覆盖**：\n  - 空 history + 无 tools\n  - history 截断：构造 30 条消息，断言条数与边界不出现 `tool` 孤儿\n  - tools_json：对 OpenAI/Anthropic 分别检查 shape（字段名与层级）\n  - context 注入：断言进入 system 而非 user\n  - image 注入：断言 user `content` 变成 parts\n- **CMake**：在 `agent_framework/CMakeLists.txt` 的 `if(BUILD_TESTING)` 下新增 `test_prompt_renderer_wp4` 并 `add_test(NAME prompt_renderer_wp4 ...)`。\n\n## 4. 测试与验收（DoD）\n- `PromptRenderer::render` 对下列组合均有单测：\n  - `tools` 空/非空\n  - `context` 空/非空\n  - `history` 空/超限\n  - `image_data` 空/非空\n- 截断行为有“成对约束”验证：不会出现 `tool` 开头的 history（孤儿）。\n- `tools_json` 的两种形态与 `phase-1-wp4.md` 的字段命名一致。\n\n## 5. 风险与缓解\n- **Message 类型不包含 tool_calls/tool_call_id**：首版用启发式识别 tool 组；若后续 WP1.5 强化历史结构，可再收紧规则。\n- **实现分散/重复**：先做 T0 结构收敛，保证三个 formatter 源文件不再是空壳。\n+\n*** End Patch"}%0A
