# Claude Code v2.1.88 `src` 工具体系源码分析

> 分析对象：`/home/Mapoet/projects/claude-code-source-code/src`  
> 源码版本：`@anthropic-ai/claude-code-source` 2.1.88（反编译研究版）  
> 分析日期：2026-08-13  
> 结论口径：以下内容描述当前仓库可见源码。编译期裁剪、Anthropic 服务端实现和缺失模块不会被推断为已经可运行。

## 1. 结论摘要

该目录包含约 1,902 个源码文件、35 MB 内容。工具系统由四层组成：

1. `src/tools.ts`：内置工具注册、feature flag、环境变量过滤、MCP 工具合并；
2. `src/Tool.ts`：统一的 `Tool` 协议和 `buildTool()` 工厂；
3. `src/services/tools/toolExecution.ts`：参数解析、校验、Hook、权限和实际调用；
4. `src/services/tools/StreamingToolExecutor.ts`：流式到达的工具调用排队和并发调度。

源码中可见约 40 个完整或近完整工具实现，另有一批只在 `tools.ts` 中出现的 feature-gated 工具，其实现已被构建期 DCE（dead-code elimination）裁掉。默认公开工具以文件操作、Shell、搜索、Agent、任务、Web、技能和 MCP 为主；内部 `ant`、实验性和远程自动化工具不能等同于普通发行版能力。

最重要的鉴权结论：

- `Read/Edit/Write/Glob/Grep/Bash/PowerShell/NotebookEdit/Task*` 等本地工具不需要独立 API Key，但受 Claude Code 本地权限规则、Hook、沙箱和操作系统权限约束。
- Claude 主循环始终需要一种模型访问身份：Claude.ai OAuth/订阅登录、`ANTHROPIC_API_KEY`，或 Bedrock/Vertex/Foundry 云凭据。
- `WebSearch` 使用 Anthropic Messages API 的服务端 Web Search 工具，不需要用户另配 Google/Bing/SerpAPI Key，但需要所用模型提供商支持该能力。
- `WebFetch` 直接抓取公开 URL，不需要 Key；它明确不适合需要登录的私有网页。
- MCP 工具是否需要 Key 由具体 MCP Server 决定；HTTP/SSE MCP 可以触发 OAuth，stdio MCP 通常通过其进程环境或配置自行取得凭据。
- `RemoteTrigger`、远程 CCR、文件上传等 Claude.ai 服务能力要求 Claude.ai OAuth Bearer Token 和对应组织/产品权限。

## 2. 工具的统一执行协议

### 2.1 `buildTool()` 做什么

`src/Tool.ts:783-791` 的 `buildTool(def)` 本身很薄：把 `TOOL_DEFAULTS`、默认 `userFacingName()` 和具体定义合并成一个工具对象。默认值采用保守策略：未声明时认为工具不可并发、不是只读、没有自动授权。

每个工具主要实现以下接口：

| 阶段 | 接口 | 作用 |
|---|---|---|
| 描述 | `prompt()` / `description()` | 给模型和权限 UI 提供工具说明 |
| 模式 | `inputSchema` / `outputSchema` | 用 Zod 定义和解析结构化输入输出 |
| 启用 | `isEnabled()` / `shouldDefer` | feature、运行环境和 ToolSearch 延迟加载 |
| 调度 | `isConcurrencySafe()` | 决定是否可以与相邻工具并行 |
| 风险 | `isReadOnly()` / `isDestructive()` | 权限和 UI 的风险语义 |
| 校验 | `validateInput()` | 在权限询问前拒绝错误或危险参数 |
| 授权 | `checkPermissions()` | 匹配 allow/ask/deny 规则，或请求用户确认 |
| 执行 | `call()` | 执行核心算法，并支持 AbortSignal 和进度事件 |
| 序列化 | `mapToolResultToToolResultBlockParam()` | 转换为 Anthropic API 的 `tool_result` |

### 2.2 一次工具调用的完整算法

```text
模型流式产生 tool_use(name, id, input)
  -> StreamingToolExecutor 根据 name 查找工具
  -> inputSchema.safeParse(input)
  -> 计算 isConcurrencySafe(input)，加入 FIFO 队列
  -> toolExecution 再做严格 schema 解析
  -> validateInput(input, context)
  -> 对 Bash 提前启动安全分类器（与后续步骤并行）
  -> PreToolUse Hooks：可追加消息、拒绝、批准或修改 input
  -> 工具权限规则 + 自动分类器 + 用户权限对话框
  -> 权限系统可能再次产生 updatedInput
  -> tool.call(finalInput, context, canUseTool, parentMessage, onProgress)
  -> PostToolUse / failure hooks、遥测、文件历史和状态更新
  -> outputSchema / mapToolResult... 转为 tool_result
  -> tool_result 回到下一轮模型上下文
```

关键性质：

- 校验先于权限提示，错误参数不会诱发无意义授权；
- PreToolUse Hook 可以修改参数，因此真正执行的输入未必等于模型原始输入；
- Bash 的 allow-classifier 被投机并行启动，以降低权限等待延迟；
- 内部 `_simulatedSedEdit` 字段会被主动剥离，防止模型伪造权限系统专用状态；
- 只有当当前执行项全都标记为 concurrency-safe 时，另一个安全工具才会并行；写工具或 Shell 等非安全工具形成顺序屏障；
- AbortController 贯穿文件搜索、网络、Shell 和 Agent，可由用户中断。

### 2.3 工具池如何生成

`src/tools.ts:208-279` 的 `getAllBaseTools()` 是内置工具总表；`getTools()` 再按环境和权限过滤；`assembleToolPool()` 将内置工具与 MCP 工具合并并按名称去重。内置工具在同名冲突时优先。

特殊过滤规则包括：

- `CLAUDE_CODE_SIMPLE`：仅保留 Bash、Read、Edit；
- 内嵌 bfs/ugrep 可用时，不再暴露专用 Glob/Grep；
- `ENABLE_LSP_TOOL` 且 LSP 已连接时才有 LSP；
- PowerShell 只在检测为可用的平台/环境启用；
- ToolSearch 开启时，`shouldDefer` 工具可先从主提示中移除，需要时再检索；
- blanket deny 会在工具发送给模型之前就移除它；
- MCP 工具按 `mcp__<server>__<tool>` 命名并动态加入；
- REPL 模式会隐藏被 REPL 包装的原始工具。

## 3. 完整工具清单与可用条件

### 3.1 默认或常规内置工具

| 模型侧名称 | 源码实现 | 核心用途 | 默认 Key 需求 |
|---|---|---|---|
| `Read` | `tools/FileReadTool/FileReadTool.ts` | 文本、图片、PDF、Notebook 读取 | 无 |
| `Write` | `tools/FileWriteTool/FileWriteTool.ts` | 新建或完整覆盖文件 | 无 |
| `Edit` | `tools/FileEditTool/FileEditTool.ts` | 基于旧字符串的精确局部替换 | 无 |
| `NotebookEdit` | `tools/NotebookEditTool/NotebookEditTool.ts` | 插入、替换、删除 ipynb 单元格 | 无 |
| `Glob` | `tools/GlobTool/GlobTool.ts` | 按通配符枚举文件 | 无 |
| `Grep` | `tools/GrepTool/GrepTool.ts` | ripgrep 正则内容搜索 | 无 |
| `Bash` | `tools/BashTool/BashTool.tsx` | POSIX Shell 命令和后台任务 | 无 |
| `PowerShell` | `tools/PowerShellTool/PowerShellTool.tsx` | PowerShell 命令和后台任务 | 无 |
| `Agent`（兼容旧名 `Task`） | `tools/AgentTool/AgentTool.tsx` | 启动同步、异步或隔离子代理 | 复用主模型凭据 |
| `AskUserQuestion` | `tools/AskUserQuestionTool/AskUserQuestionTool.tsx` | 向交互用户收集结构化选择 | 无 |
| `SendMessage` | `tools/SendMessageTool/SendMessageTool.ts` | Agent/团队间消息传递 | 无；远程模式另论 |
| `TaskCreate/Get/List/Update` | 对应 `tools/Task*Tool` | 任务图和状态管理 | 无 |
| `TaskStop` | `tools/TaskStopTool/TaskStopTool.ts` | 终止 Shell、Agent 或远程任务 | 无；远程任务需远端身份 |
| `TaskOutput` | `tools/TaskOutputTool/TaskOutputTool.tsx` | 读取/阻塞等待后台任务输出 | 无 |
| `TodoWrite` | `tools/TodoWriteTool/TodoWriteTool.ts` | 旧版会话 Todo 状态 | 无 |
| `Skill` | `tools/SkillTool/SkillTool.ts` | 加载并执行技能/斜杠命令 | 技能自身可能需要 |
| `ToolSearch` | `tools/ToolSearchTool/ToolSearchTool.ts` | 检索延迟注册工具 | 无 |
| `WebFetch` | `tools/WebFetchTool/WebFetchTool.ts` | 获取公开网页并转 Markdown | 无独立 Key |
| `WebSearch` | `tools/WebSearchTool/WebSearchTool.ts` | Anthropic 服务端联网搜索 | 复用模型身份 |
| `EnterPlanMode` / `ExitPlanMode` | 对应 PlanMode 工具 | 切换规划/执行权限状态 | 无 |
| `EnterWorktree` / `ExitWorktree` | 对应 Worktree 工具 | 创建、进入和退出 Git 隔离工作树 | 无；需要 Git |
| `SendUserMessage`（旧名 `Brief`） | `tools/BriefTool/BriefTool.ts` | 主动向用户发送简报/附件 | 本地无；上传需 OAuth |
| `ListMcpResourcesTool` | 对应同名目录 | 枚举 MCP Resource | 由 MCP Server 决定 |
| `ReadMcpResourceTool` | 对应同名目录 | 读取指定 MCP Resource URI | 由 MCP Server 决定 |

### 3.2 条件、内部和测试工具

| 工具 | 启用条件 | 证据边界 |
|---|---|---|
| `LSP` | `ENABLE_LSP_TOOL`，插件提供 LSP 配置且成功连接 | 实现完整 |
| `Config` | `USER_TYPE=ant` | Anthropic 内部配置工具 |
| `Tungsten` | `USER_TYPE=ant` | 内部工具；普通发行版不可假设可用 |
| `REPL` | `USER_TYPE=ant` 且 REPL 模式 | 当前 `src/tools/REPLTool` 只有 constants/primitive stubs，主体引用缺失 |
| `CronCreate/Delete/List` | 编译 feature `AGENT_TRIGGERS` | 本地调度工具实现可见 |
| `RemoteTrigger` | 编译 feature `AGENT_TRIGGERS_REMOTE` | 实现可见，调用 Claude.ai CCR API |
| `StructuredOutput` | 特殊结构化输出流程 | 内部合成工具，不执行外部动作 |
| `TestingPermissionTool` | `NODE_ENV=test` | 测试专用 |
| `Sleep` | `PROACTIVE` 或 `KAIROS` | 仅 prompt 文件可见，主体被裁剪 |

`tools.ts` 还引用但当前 `src/tools` 中没有实现目录的工具包括：`SuggestBackgroundPRTool`、`MonitorTool`、`SendUserFileTool`、`PushNotificationTool`、`SubscribePRTool`、`VerifyPlanExecutionTool`、`OverflowTestTool`、`CtxInspectTool`、`TerminalCaptureTool`、`WebBrowserTool`、`SnipTool`、`ListPeersTool`、`WorkflowTool`。这些只能证明 v2.1.88 的注册层知道它们，不能证明本仓库能构建或运行它们。

## 4. 各类工具的具体算法

## 4.1 Read：多模态文件读取

输入为 `file_path`，以及可选 `offset`、`limit`、PDF `pages`。

算法流程：

1. Zod 校验字段；PDF 页码解析为 1-based 区间，并限制单次页数。
2. 展开 `~`、相对路径和平台分隔符；在任何 I/O 前匹配 read-deny 规则。
3. UNC 路径暂不做 `stat/read`，防止 Windows SMB/NTLM 凭据泄露；交给权限层决定。
4. 按扩展名拒绝普通二进制文件，但放行图片、PDF 和 SVG；同时封禁 `/dev/random`、`/dev/zero`、stdio fd 等可能无限输出或阻塞的设备。
5. 检查 `readFileState`：同一文件、同一范围、mtime 未改变时返回“未变化”桩，避免把相同内容反复塞进上下文。
6. 按文件类型分派：
   - 文本：按 offset/limit 读取，检测编码和换行，添加行号；
   - Notebook：解析 JSON，把 cell 映射为适合模型阅读的文本；
   - 图片：识别格式和尺寸，按 token 预算缩放、降采样、压缩，返回 base64 image block；
   - PDF：读取页数，按指定范围提取；小 PDF 可内联，较大 PDF 分页抽取。
7. 对文本先快速估算 token；接近阈值时调用 count-tokens 路径精算，超过上限报错而非无界返回。
8. 将内容、mtime、读取区间写入 `readFileState`，供 Edit/Write 的“先读后写”和并发修改检测使用。

默认文本上限是总文件大小 256 KB、实际输出 25,000 tokens；前者在读取前按整个文件检查（即使请求了切片），后者在读取后检查输出。token 上限优先级为 `CLAUDE_CODE_FILE_READ_MAX_OUTPUT_TOKENS` → GrowthBook 动态配置 → 25,000 硬编码默认值。

基础依赖：Node `fs/promises`、`path`、Zod、项目内 `imageResizer`、PDF/Notebook 解析器、Anthropic image block 类型。无需额外 Key；精确 token 计数可能复用现有 Anthropic API 客户端，但不是一种新凭据。

## 4.2 Edit：精确字符串替换

输入是 `file_path`、`old_string`、`new_string` 和可选 `replace_all`。

算法流程：

1. 展开绝对路径并匹配 edit deny/allow/ask 规则。
2. 拒绝向团队 memory 文件写入检测到的 secrets。
3. 要求文件已被 Read；部分读取仅在能证明编辑目标位于已读内容时放行。
4. 比较当前 mtime 与 `readFileState.timestamp`；若时间变新，再对完整读取内容做内容比对以减少云同步/杀毒软件造成的假阳性。
5. 读取内容和编码，规范化 CRLF 后执行替换匹配。匹配器除精确匹配外，还处理若干缩进/换行差异；如果 `replace_all=false` 且出现多处匹配则拒绝，避免误改。
6. 在“最后一次陈旧性检查”和同步写盘之间不执行 `await`，形成单进程内的 read-modify-write 临界区。
7. 保留原编码和换行策略，生成 structured patch；记录 file history 备份。
8. 更新 read cache，通知 VS Code diff、LSP `didChange/didSave`、诊断追踪器和文件操作遥测。

这不是基于 AST 的重构算法，而是有陈旧性保护的确定性文本替换。它不需要 Key，但需要目标目录写权限和 Claude Code edit 授权。

## 4.3 Write：完整文件替换

Write 与 Edit 共用“权限规则 → 先读后写 → mtime/content 双重陈旧检测 → 历史备份 → 同步落盘 → LSP/IDE 通知”的框架。

差异是：

- 文件不存在时允许直接创建并递归创建父目录；
- 文件已存在时必须先完整 Read，不能基于 partial view 覆盖；
- 内容是完整替换，不尝试保留旧换行；源码明确使用模型给出的内容并写为 LF，避免旧 CRLF 或仓库采样误导；
- 更新已有文件时生成 old→new 的完整 structured patch；新建时返回 create 类型结果。

## 4.4 NotebookEdit：结构化修改 ipynb

算法流程：

1. 强制 `.ipynb` 后缀，校验 `replace/insert/delete`；insert 必须给 cell type。
2. 要求先 Read 且 mtime 未改变。
3. JSON 解析 notebook；按真实 cell `id` 查找，失败后兼容 `cell-N` 数字索引。
4. insert 插在指定 cell 后；无 cell_id 时插在开头；replace 超过尾部的兼容路径可转插入；delete 用 `splice` 删除。
5. 修改 source、cell_type 等结构后序列化，保留原编码和行尾风格。
6. 更新 read cache、历史、变更行统计和 UI diff。

它只编辑 Notebook JSON，不启动 Jupyter Kernel，也不执行单元格，因此不需要 Jupyter Token 或 Python Key。

## 4.5 Glob 与 Grep

### Glob

1. 校验 path 存在且为目录；UNC 同样延迟 I/O。
2. 用 filesystem read 权限规则判断路径。
3. 调用项目 `utils/glob.js` 遍历，接受 pattern、root、limit、offset 和 AbortSignal。
4. 默认最多返回 100 项（上下文可覆盖），结果转为相对 cwd 的路径并标注 truncated。

### Grep

1. 接收 regex、path、glob/type、大小写、多行、上下文行、输出模式、limit/offset。
2. 自动排除 `.git/.svn/.hg/.bzr/.jj/.sl` 等 VCS 目录以及插件缓存噪声。
3. 把输入转换为 ripgrep 参数；多行模式使用 `-U --multiline-dotall`。
4. 调用 `utils/ripgrep.ts`。优先级是用户指定的 system rg、Bun 内嵌 rg、随包 vendor rg；支持流式读取、超时和 Abort 后强制终止。
5. 解析为 content、files_with_matches 或 count；默认 `head_limit=250`，`0` 才表示无限；最后相对化路径。

两者只读、可并发、不需要 Key。Grep 需要可用的 ripgrep；正式 bundle 通常自带，研究仓库直接运行时要确认 vendor 或系统 `rg`。

## 4.6 Bash 与 PowerShell：安全 Shell 执行器

Shell 工具不是简单的 `child_process.exec()` 包装，而是“语义解析 + 权限分类 + 沙箱 + 任务生命周期”系统。

算法流程：

1. Zod 校验 command、timeout、description、`run_in_background`、`dangerouslyDisableSandbox`。
2. 解析命令链和重定向：Bash 使用项目 Shell AST/`shell-quote` 辅助逻辑；PowerShell 使用专门 tokenizer/parser、cmdlet 语义和 common parameter 表。
3. 安全检查包括：
   - 命令替换、变量展开、重定向目标、管道和多命令边界；
   - 破坏性命令提示；
   - Git 子命令和参数的只读/写入区分；
   - 路径是否跨越允许工作目录；
   - sed 是否可安全模拟为 Edit；
   - PowerShell `--%`、调用运算符、危险 cmdlet、环境变量注入等特有风险。
4. 权限判定按 deny → ask → allow/prefix → 只读识别 → 自动分类器执行。复杂或无法可靠解析的命令趋向询问，而不是自动放行。
5. 根据设置决定沙箱：Linux 常用 bubblewrap，macOS 使用 `sandbox-exec`；PowerShell 在 Linux/macOS/WSL 可被同样包装，原生 Windows 没有该 POSIX 沙箱。若企业策略强制沙箱且禁止 unsandboxed，原生 Windows PowerShell会拒绝执行。
6. `utils/Shell.exec()` 启动子进程，流式收集 stdout/stderr，周期发送 progress；支持 timeout 和 Abort。
7. 显式后台运行或长任务自动后台化时，注册 `LocalShellTask`，返回 task id；`TaskOutput` 继续读输出，`TaskStop` 可终止。
8. 用户在前台命令运行时发新消息，系统通常将任务后台化而不是直接杀掉；真正中断时终止进程组并清理轮询。

依赖：Node `child_process`、Shell/PowerShell 可执行文件、Git（仅相关命令）、可选 bwrap/sandbox-exec。运行任意具体命令时，还依赖该命令本身，例如 npm、Python、Docker；这些不是 BashTool 的固定依赖。Shell 本身不需要 Key，但命令内部访问云服务时需要该命令自己的凭据。

## 4.7 Agent、团队与任务系统

### Agent

Agent 输入包含 prompt、agent type、model、同步/后台模式、resume id、可选 name、team、isolation 等。

执行流程：

1. 从内置 agent 和 `.claude/agents`/插件定义中解析 agent type、模型、系统提示、工具 allowlist/denylist、最大 turns 和权限模式。
2. 选择 fresh、resume 或 fork；为子代理创建独立消息上下文、AbortController、token/tool 统计和 agent id。
3. 根据定义过滤工具。异步 Agent 无法显示交互授权 UI，因此会避免需要用户提示的操作。
4. `isolation=worktree` 时创建临时 Git worktree；无改动可自动清理，有改动则返回路径/分支供主代理处理。
5. 同步模式通过 async generator 将子代理消息和进度汇入当前 tool call；后台模式创建 `LocalAgentTask`，立即返回 task id。
6. 子代理内部继续运行同一 query/tool loop，因此仍会产生模型 API 调用、计费和 token 消耗。
7. 完成后汇总最终文本、usage、tool count、duration；异常/取消时关闭 generator，并终止该 Agent 启动的后台 Shell 任务。

Agent 没有单独的 Key，但每个子代理复用主会话模型身份并增加模型调用成本。remote isolation/CCR 属于内部或远程能力，需要 Claude.ai OAuth 与相应权限。

### Task 状态工具

- `TaskCreate`：创建 subject/description/activeForm 节点；
- `TaskGet`：按 id 返回节点及依赖；
- `TaskList`：枚举任务状态；
- `TaskUpdate`：更新 pending/in_progress/completed、owner、metadata 和依赖边；
- `TaskOutput`：对 local_bash、local_agent、remote task 统一读取输出，可带 timeout 阻塞轮询；
- `TaskStop`：解析 task_id（兼容旧 shell_id），按任务类型调用 cancel/kill；
- `SendMessage`：按 agent id/name/team 路由普通消息、广播、shutdown request/response 和 plan approval 等协议消息；
- `TeamCreate/Delete`：创建团队目录/配置和成员状态，删除前检查活跃成员。

这些主要依赖 AppState、任务对象、文件持久化和进程控制，不需要独立 Key。

## 4.8 Skill

SkillTool 的核心不是“执行一个 Markdown 文件”，而是将技能定义转换为当前会话的新消息或隔离 Agent 调用：

1. 从内置 commands、用户/项目技能、插件技能中按 name 找定义；
2. 校验参数和命令是否允许模型调用；
3. 读取技能正文，执行变量替换、动态上下文和附件解析；
4. 普通技能把处理后的 user/system/attachment messages 标记为该 tool use 的瞬态消息，注入主循环；
5. 配置为 fork/agent 的技能在隔离 Agent 内运行，并应用独立 token budget、model 和工具列表；
6. 插件技能携带 marketplace/plugin telemetry，但第三方技能内容和动作不由 SkillTool 本身保证安全。

SkillTool 本身不需要 Key；技能调用 Web、数据库、云 CLI 或 MCP 时，需要相应外部授权。

## 4.9 ToolSearch：延迟工具检索算法

ToolSearch 只搜索当前进程中 `shouldDefer` 的工具，不访问互联网。

检索流程：

1. 根据延迟工具名称集合构造 cache key；集合变化则清空描述 memoize cache。
2. `select:A,B` 走直接选择，支持多选和大小写兼容。
3. 普通 query 先查完全同名，再查 `mcp__server` 前缀。
4. 将 CamelCase、下划线和 MCP 双下划线拆成词项；`+term` 表示必须匹配。
5. 对每个候选获取 memoized prompt/description，并按下列权重计分：
   - 名称词精确匹配：普通 10，MCP 12；
   - 名称子串：普通 5，MCP 6；
   - `searchHint` 单词匹配：4；
   - 描述单词边界匹配：2；
   - full-name fallback：3。
6. 过滤零分，降序排序并截取 `max_results`（默认 5）；同时报告仍在连接的 MCP Server。

## 4.10 LSP

LSP 支持 definition、references、hover、document/workspace symbols、implementation 和 call hierarchy。

流程：

1. 严格校验 operation、file、1-based line/character；文件必须存在、为普通文件且小于限制。
2. 匹配 read 权限；等待 LSP manager 初始化。
3. manager 根据插件配置和文件扩展名选择 server，按需启动其进程并发送 initialize/initialized。
4. 将 1-based 坐标转为 LSP 的 0-based position，调用相应 `textDocument/*` 或 `workspace/*` 请求。
5. 对 `ContentModified(-32801)` 等瞬态错误有限重试。
6. 格式化 URI、range、symbol/call hierarchy 为文本结果；编辑工具另行发送 didChange/didSave，异步 diagnostics 进入诊断注册表。

依赖：`vscode-languageserver-types`、`vscode-jsonrpc`（按需加载）以及插件声明的实际 language server 可执行文件。没有统一 LSP Key，但某些商业 language server 可能有自己的授权要求。

## 4.11 WebFetch

1. Zod 验证 URL 和 prompt；权限粒度转换成 `domain:<hostname>`。
2. 内建白名单域可直接允许，否则依次匹配 deny、ask、allow，并可建议把域加入 local settings。
3. `getURLMarkdownContent()` 发出 HTTP 请求，执行 SSRF/URL 安全、内容类型、大小、HTML→Markdown 和截断处理。
4. 若 30x 跳到不同 host，不自动跟随，而是返回新 URL，要求再次调用以触发新域权限判断。
5. 将 Markdown 与 prompt 交给内容处理路径，返回经过提炼的文本、状态码、字节数和耗时。

它只适用于公开内容；源码 prompt 明确说明认证型 Google Docs、Confluence、Jira、GitHub 等应使用专门 MCP。无需独立 Key，但受网络、代理、防火墙和域权限限制。

## 4.12 WebSearch

WebSearch 不是本地爬虫，也没有调用 Google/Bing SDK。它构造 Anthropic beta server tool：

```text
{ type: "web_search_20250305", name: "web_search",
  allowed_domains?, blocked_domains?, max_uses: 8 }
```

流程：

1. query 至少 2 字符，allowed_domains 与 blocked_domains 不能同时给出。
2. 创建一个内部 user message，调用 `queryModelWithStreaming()`；模型是主模型或小型快速模型策略选择。
3. Anthropic/兼容提供商在服务端执行最多 8 次搜索。
4. 流式解析 `server_tool_use`、`web_search_tool_result`、text/citation blocks。
5. 把搜索 hit 归一化为 title/url，并保留模型文字说明和耗时。

启用条件：first-party 始终支持；Vertex 仅 Claude 4 系列；Foundry 按源码假定其上架模型支持；Bedrock 分支未启用。用户不需要搜索引擎 Key，但必须有支持 Web Search 的模型访问身份，服务端还可能按供应商策略计费或限制。

## 4.13 MCP 工具和 MCP OAuth

`MCPTool.ts` 只是模板。连接 MCP Server 后，client 会为每个远端工具覆盖：

- 名称：`mcp__<server>__<tool>`；
- 描述和 JSON Schema：来自 MCP `tools/list`；
- `call()`：通过 MCP connection 发送 `tools/call`；
- 进度、权限、超时、结果 content block 和大结果截断。

资源工具：

- `ListMcpResourcesTool` 读取已连接 server 的 resources 列表；
- `ReadMcpResourceTool` 按 server + URI 调用 MCP `resources/read`；
- 资源和工具是两套协议能力，服务器可以只实现其中一种。

未认证 HTTP/SSE Server 会被替换为 `mcp__<server>__authenticate` 伪工具。调用后：

1. `performMCPOAuthFlow(skipBrowserOpen)` 做 OAuth discovery/PKCE；
2. 立即返回 authorization URL 交给用户；
3. callback 在后台完成 token exchange 和安全存储；
4. 清理 auth cache、重新连接 server；
5. 用真实 `mcp__server__*` 工具替换 authenticate 伪工具。

stdio transport 不支持该伪工具 OAuth 流程，需用户运行 `/mcp` 或按 server 文档配置环境变量。任何 GitHub、Slack、数据库等 Key 都属于相应 MCP Server，不属于 Claude Code 的统一必需项。

## 4.14 Plan、Worktree、AskUserQuestion、Brief 和 Cron

- `EnterPlanMode`：保存原权限模式并切为 plan；限制写操作，允许只读调查。
- `ExitPlanMode`：携带 plan/allowed prompts 等状态，请用户批准后恢复执行权限；它不会自己执行计划。
- `EnterWorktree`：检查 Git 仓库、名称和状态后创建隔离 worktree/branch，切换会话 cwd；`ExitWorktree` 汇总改动并返回原目录，是否保留取决于状态和用户选择。
- `AskUserQuestion`：校验问题和选项，在交互 UI 中暂停；用户回答写入 tool result。headless/SDK 必须由宿主实现交互回调，否则不能凭空回答。
- `SendUserMessage/Brief`：组织简短消息和附件；本地附件可展示，桥接上传到 `/api/oauth/file_upload` 时需要 Claude.ai OAuth token。
- `CronCreate/Delete/List`：维护会话内调度项，校验 cron 表达式/频率、注册唤醒任务并提供枚举和删除；由 `AGENT_TRIGGERS` 编译特性控制。
- `RemoteTrigger`：刷新 Claude.ai OAuth token，取得 organization UUID，使用 Bearer token 调 CCR remote-trigger API；明确需要 Claude.ai 登录及远程 Agent 产品权限。

## 5. 基础库和运行时依赖

该研究仓库的 `package.json` 只列 `typescript` 和 `esbuild`，因为原始 Claude Code 发布物是自包含 bundle；源码中的真实运行依赖远多于 lockfile。不能把当前 `package-lock.json` 当成完整生产依赖表。

### 5.1 关键库

| 库/模块 | 用途 |
|---|---|
| `zod/v4` | 所有工具 input/output schema、严格解析和错误报告 |
| `@anthropic-ai/sdk` | Messages 流、tool_use/result、图片 block、Web Search beta、模型调用 |
| `@modelcontextprotocol/sdk` | MCP stdio/SSE/Streamable HTTP、OAuth、resources/tools 协议 |
| `react` + `ink` | 终端权限对话框、进度、diff、工具结果 UI |
| `lodash-es` | memoize、uniqBy、reject、merge 等数据操作 |
| `execa` / Node `child_process` | 外部命令和语言服务器进程 |
| `shell-quote` + 项目 Shell AST | Bash 解析、参数和重定向安全分析 |
| `vscode-languageserver-types` / `vscode-jsonrpc` | LSP 类型与 JSON-RPC |
| `axios` / Node HTTP(S) | 部分网络与 API 请求 |
| `chokidar` | 文件/配置监控 |
| `lru-cache` | 文件、配置和解析缓存 |
| `picomatch` / `ignore` | glob、ignore 和权限路径匹配 |
| `diff` / `jsonc-parser` / `marked` / `highlight.js` | diff、配置和 Markdown/UI |
| OpenTelemetry packages | trace、metrics、logs；不是工具执行必需 Key |
| `@aws-sdk/client-bedrock-runtime` | Bedrock provider |
| `google-auth-library` | Vertex AI ADC 凭据 |
| Anthropic 私有 `@ant/*` 与 N-API 模块 | Computer Use、Chrome、图片/终端/颜色等 bundle 能力 |

### 5.2 外部程序

| 程序 | 是否必需 | 用途 |
|---|---|---|
| Node.js >=18 / 正式 bundle 的 Bun runtime | 核心 | CLI 与工具运行时 |
| `rg` | Grep 必需 | 正式 bundle 通常内嵌或携带，源码运行可用系统 rg |
| `bash` | Unix BashTool | 命令执行 |
| `pwsh`/PowerShell | PowerShellTool | 仅该工具需要 |
| Git | 条件必需 | worktree、diff、Agent isolation、Git 命令 |
| bwrap 或 `sandbox-exec` | 可选但重要 | Linux/macOS Shell 沙箱 |
| 各语言服务器 | LSP 条件必需 | 由插件配置，例如 pyright、gopls、rust-analyzer |
| Python/Jupyter | 非 NotebookEdit 必需 | 只有真正执行 notebook 或 Python 命令时才需要 |

## 6. Key、登录和授权矩阵

### 6.1 主模型访问：选择一种有效身份，而非全部都要

| 模式 | 典型配置 | 需要什么 |
|---|---|---|
| Claude.ai 订阅 | `claude auth login` / `/login` | 浏览器 OAuth，access/refresh token 安全保存；订阅及组织权限 |
| Anthropic API | `ANTHROPIC_API_KEY` | Anthropic Console API Key 和可用额度 |
| Bearer/代理 | `ANTHROPIC_AUTH_TOKEN`、`CLAUDE_CODE_OAUTH_TOKEN`、`apiKeyHelper` | 由企业网关或宿主提供的 token/helper；可能需自定义 `ANTHROPIC_BASE_URL` |
| Amazon Bedrock | `CLAUDE_CODE_USE_BEDROCK=1` | AWS 标准凭据链：profile、环境变量、IAM role、SSO/STS 等，以及 Bedrock model access |
| Google Vertex AI | `CLAUDE_CODE_USE_VERTEX=1` | Google ADC，如 `GOOGLE_APPLICATION_CREDENTIALS`、gcloud ADC 或 workload identity；项目、区域、Vertex 权限 |
| Microsoft Foundry | `CLAUDE_CODE_USE_FOUNDRY=1` | `ANTHROPIC_FOUNDRY_API_KEY`，或 Azure `DefaultAzureCredential` 对 Cognitive Services scope 获取的 AAD token；同时需要 Foundry endpoint/deployment 配置 |

源码的优先级和受管环境逻辑较复杂：bare 模式只接受隔离范围内的 Key/helper；homespace/managed OAuth 会避免错误混用外部 `ANTHROPIC_API_KEY`；OAuth token 过期时用 refresh token 和文件锁刷新，防止多进程竞争。

### 6.2 工具级授权

| 能力 | 额外 Key/授权 |
|---|---|
| 本地文件、Glob/Grep、NotebookEdit | 无 Key；Claude Code allow/ask/deny + OS 文件权限 |
| Bash/PowerShell | 无统一 Key；Claude Code 命令规则、沙箱、OS 权限；命令访问的外部系统自行授权 |
| Agent/Skill | 复用主模型身份；子代理增加调用成本；技能外部依赖自行授权 |
| WebFetch | 无 Key；仅公开 URL、域权限、网络/代理 |
| WebSearch | 无搜索引擎 Key；要求 provider/model 支持 Anthropic server Web Search |
| MCP | 每个 Server 自己的 OAuth/API Key/本地凭据；Claude Code 只编排和安全保存 |
| LSP | 通常无 Key；商业 language server 例外 |
| Git worktree | 无 Key；若 Shell 内 push，则需要 Git remote SSH key/PAT |
| RemoteTrigger/CCR/Brief 上传 | Claude.ai OAuth token、组织 UUID、相应产品 entitlement |
| `ant` 内部工具 | Anthropic 内网身份、构建标志和服务权限；普通用户无法靠设置环境变量可靠获得 |

## 7. 权限与安全机制

### 7.1 三层授权

1. **工具可见性**：feature flag、环境、deny blanket rule 先决定模型是否看得到工具；
2. **工具输入授权**：按工具名、路径、命令 prefix、Web domain、MCP server/tool 匹配 allow/ask/deny；
3. **操作系统/远端授权**：即使 Claude Code allow，文件 ACL、沙箱、云 IAM、OAuth scope 和服务端 policy 仍可拒绝。

### 7.2 关键安全设计

- Read-before-write + mtime/content 检测，避免覆盖用户或 formatter 的并发修改；
- UNC 路径在授权前不做 I/O，避免 Windows NTLM 自动认证泄漏；
- Shell 使用语义分析而非只做字符串黑名单；未知结构默认 ask；
- WebFetch 跨域 redirect 必须重新授权；
- MCP token 在 OAuth/secure storage 路径处理，不注入模型或 Shell；
- 后台 Agent 无交互能力时避免权限提示，而不是自动同意；
- 工具详细参数默认不进入遥测；`OTEL_LOG_TOOL_DETAILS=1` 会扩大日志敏感度，应谨慎启用；
- 工具输出有字符/token 上限、持久化阈值和截断，避免单次结果挤满上下文。

## 8. 当前仓库的可构建性与证据边界

1. 这是从 Claude Code 2.1.88 bundle 还原的研究源码，不是官方开发仓库。
2. `package.json` 的依赖不完整；大量外部包来自原 bundle，不能仅执行 `npm install && tsc` 就认为获得生产构建。
3. `feature()` 来自 `bun:bundle` 编译期宏；不同发行构建会物理裁掉不同代码。
4. `USER_TYPE=ant` 不只是一个普通开关，还隐含内部模块、端点、身份和 entitlement。
5. `tools.ts` 中缺少目录的工具只能记录为“注册引用/未来或内部能力”，不能分析不存在的 `call()` 算法。
6. Web Search 的搜索和排序主体在模型提供商服务端，源码只展示请求 schema、流式 block 解析和结果归一化。
7. MCP 的业务算法在外部 Server；Claude Code 只负责 discovery、schema、transport、授权、调用和结果编排。

## 9. 源码导航索引

建议按以下顺序继续审阅：

1. `/home/Mapoet/projects/claude-code-source-code/src/tools.ts`：总注册表和 feature gates；
2. `/home/Mapoet/projects/claude-code-source-code/src/Tool.ts`：Tool 类型和默认行为；
3. `/home/Mapoet/projects/claude-code-source-code/src/services/tools/toolExecution.ts`：真实执行管线；
4. `/home/Mapoet/projects/claude-code-source-code/src/services/tools/StreamingToolExecutor.ts`：并发队列；
5. `/home/Mapoet/projects/claude-code-source-code/src/tools/*/*Tool.ts(x)`：逐工具实现；
6. `/home/Mapoet/projects/claude-code-source-code/src/utils/permissions/`：权限规则；
7. `/home/Mapoet/projects/claude-code-source-code/src/tools/BashTool/` 与 `PowerShellTool/`：Shell 安全算法；
8. `/home/Mapoet/projects/claude-code-source-code/src/services/mcp/`：MCP transport/OAuth；
9. `/home/Mapoet/projects/claude-code-source-code/src/services/lsp/`：LSP 生命周期；
10. `/home/Mapoet/projects/claude-code-source-code/src/utils/auth.ts` 与 `services/api/client.ts`：模型身份和 provider 选择。

## 10. 一句话判断某工具是否需要 Key

先问“它的 `call()` 是本地计算、调用主模型，还是访问独立远端服务”：

- 本地计算/文件/进程：不需要 Key，只需要本地授权；
- 再调用 Claude 模型：复用主模型 OAuth/API/云身份；
- MCP 或独立远端服务：需要该服务自己的 OAuth/Key；
- Claude.ai CCR/上传等产品 API：需要 Claude.ai OAuth 和 entitlement；
- feature-gated 且实现缺失：不能从当前仓库确认其真实授权需求。
