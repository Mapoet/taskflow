### 目标（按你的新需求）
在现有 WP1.4 `PromptRenderer` 基础上新增“**用户模板变量**”能力：

- **两阶段渲染顺序（锁定）**
  1) 先对用户提供的 `system_prompt` / `user_prompt` 中的 `{{var}}` 做“用户变量”渲染  
  2) 再进入现有 PromptRenderer 的“内置关键词渲染”（`system_prompt/user_prompt/context/tools_text` 等）

- **变量缺失时不报错**：而是在提示词里**显式说明缺失变量**，并追加一段提示词交给模型决定如何向用户追问/澄清。

- **支持自定义变量参数**：你已选定 **在 `LLMInput` 增加 `extra_variables: map<string,string>`** 作为传入方式。

- **缺失变量提示放置位置**：你已选定 **新增一条独立 system 消息**（`extra_system_msg`）。

---

### 需要改的接口与文件（范围最小、可落地）
- **类型改动**：`agent_framework/include/agent/types.hpp`
  - 在 `struct LLMInput` 增加：`std::map<std::string, std::string> extra_variables;`
- **模板能力增强（内部工具）**：`agent_framework/src/prompt_renderer/prompt_template.cpp`
  - 利用现有 `StringPromptTemplate::extract_variables()` / `validate_variables()`，新增一个**可复用的小工具函数**（放在 internal/匿名 namespace）：
    - `scan_template_vars(std::string_view text) -> vector<string>`
    - `render_user_template(std::string text, const map<string,string>& user_vars, out missing_vars) -> string`
  - 注意：这一步是“用户变量渲染”，与内置 vars 的渲染解耦。
- **渲染主流程改造**：`agent_framework/src/prompt_renderer/prompt_renderer.cpp`
  - 在构造 `vars["system_prompt"] / vars["user_prompt"]` 之前：
    - 对 `input.system_prompt` 和 `input.user_prompt` 先做用户变量替换（使用 `input.extra_variables`）
    - 记录缺失变量集合 `missing_vars`
  - 如果 `missing_vars` 非空：
    - 在 `RenderedPrompt.messages` 中插入 **一条独立 system 消息**（位置建议：放在“主 system message”之后、history 之前），内容包括：
      - 明确列出缺失变量名（例如 `missing: {{foo}}, {{bar}}`）
      - 指示模型：需要向用户询问这些变量或在无法获取时选择合理默认/继续对话
  - 然后继续现有逻辑：context 注入到 system、history truncate、tools_json、multimodal parts 等。
- **测试更新**：`agent_framework/tests/test_prompt_renderer_wp4.cpp`
  - 新增用例：
    - **complete_render**：`system_prompt="Hi {{name}}"` + `extra_variables{name=Mapoet}` → 输出 system 中应出现 `Hi Mapoet`
    - **missing_vars**：`user_prompt="Ask {{topic}}"` + `extra_variables{}` → `messages` 中应出现额外的 system 提示，列出 `topic`
    - **two_phase_order**：确保“用户变量渲染”发生在内置渲染之前（例如用户变量渲染后再拼接 context，不应把 `{{var}}` 留到后面才处理）
- **CMake**：如新增了新的 internal 头/源文件，需确保已加入 `agent_framework/CMakeLists.txt` 的 `AGENT_SOURCES`（避免再出现链接缺符号）。

---

### 关键行为细节（把歧义消掉）
- **变量扫描对象**：只扫描用户提供的 `LLMInput.system_prompt` / `LLMInput.user_prompt`（在进入模板/vars 系统之前）。
- **缺失变量的判定**：出现 `{{x}}` 且 `extra_variables` 不含 key `x`。
- **缺失变量不替换**：保留原样 `{{x}}`（便于模型与用户对齐），同时在额外 system 消息中列出缺失项。
- **安全性**：`extra_variables` 值按纯文本替换，不执行任何代码；不支持嵌套模板语法。

---

### 验收标准（DoD）
- `LLMInput.extra_variables` 生效：`{{var}}` 在 system/user 中可按用户变量正确替换
- 缺失变量时：`RenderedPrompt.messages` 中出现**独立 system 消息**明确列出缺失项，并引导模型追问用户
- 单测覆盖：全渲染/缺失/顺序（两阶段）至少 3 类
- 不破坏现有：context 注入、tools_json、history truncate、多模态注入逻辑

---

如果你还希望“缺失变量提示词”的具体措辞固定成一个模板（中英/更偏 ReAct 风格/更偏简洁），我可以把那段 system policy 的建议文本也一并写进计划里，便于你锁定行为一致性。