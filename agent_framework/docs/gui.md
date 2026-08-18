# Agent Framework GUI 重构建议与实施计划

> 实施状态：可执行升级计划维护于 [`gui-v2-plan.md`](gui-v2-plan.md)，
> 联合顺序见 [`af-term-gui-integration-plan.md`](af-term-gui-integration-plan.md)，
> 已冻结的身份/生命周期语义见
> [`gui-identity-lifecycle-rfc.md`](gui-identity-lifecycle-rfc.md)。本文保留为产品需求与缺口基线。

> 审核日期：2026-08-17
>
> 目标：把 Agent Framework（下文简称 AF）从“单 Demo 会话 + 运行态看板”升级为可管理、可协作、可并行、可审计的多用户 Agent 工作台。
>
> 参考对象：Claude Code、Codex Desktop、DeepSeek Harness（下文简称 DSH）以及 AF 当前代码与真实运行界面。

## 0. 结论先行

AF 当前需要的不是一次 CSS 美化，而是一次产品模型、运行模型和信息架构的联合重构。

当前 Web UI 把 Session 当作固定字符串 default，把整个进程的工作状态压缩成一个全局 busy 标志，同时把大量 Operations 信息常驻在主界面。这造成三个结构性问题：

1. 用户看到的 Session 并不是可查询、可创建、可打开、可归档和可删除的产品对象。
2. 后端不能表达“同一用户并行运行多个 Session”，更不能可靠表达“多个用户协作同一 Session”。
3. 主界面同时承担聊天、运行监控、模板展示、Skill 状态、MCP 状态和运维看板，信息很多，但行动入口、设置入口和对象边界都不清楚。

建议将目标产品定义为：

> 以组织、用户、项目、工作区为管理边界，以 Session 为工作入口，以 Run 为执行边界，以 Event/Artifact/Receipt 为证据边界，以 Agent Template、Skill、Workflow 为编排能力的多用户 Agent 工作台。

目标界面采用“可折叠左侧 Session 导航 + 中央会话/工作区 + 按需打开的右侧上下文抽屉”：

- 左侧负责组织、项目、工作区切换，以及 Session 的查询、新建、打开、置顶、归档、恢复和删除。
- 中央只保留当前 Session 的会话、工作成果、执行状态和输入区。
- 右侧只在用户主动打开时展示计划、活动、文件、Agent/Skill、记忆、审批或证据，不再永久占用空白区域。
- 设置与管理进入独立页面或模态流程，按照公司、用户、项目、工作区、Session、Run 分层展示有效值与来源。
- 同一用户可让多个 Session 后台并行执行；同一 Session 默认只有一个顶层主动 Run，多人输入通过 steer、排队或新 Run 明确处理。

### 0.1 建议优先级

| 优先级 | 必须完成的能力 | 原因 |
|---|---|---|
| P0 | 产品级 Session Catalog、统一身份、Session CRUD/检索、按 Session 隔离的 Run Supervisor | 不完成这些，任何新界面仍然只是 default Session 的新皮肤 |
| P0 | 新三栏壳层、真实 Session 列表、动态右侧抽屉、删除所有无响应的伪按钮 | 直接解决当前可用性问题 |
| P1 | OIDC/企业身份接入、组织/项目/工作区 RBAC、协作冲突语义 | 才能把“共享数据库”升级为真正的多用户协作 |
| P1 | 分层设置、凭据代理、记忆治理与 Run 配置快照 | 避免配置漂移、越权读取和结果不可复现 |
| P1 | Agent/Skill/Workflow 管理中心 | 支撑模型驱动、Agent 指令驱动、Workflow 节点三种业务模式 |
| P2 | 多窗格 Session、可扩展 UI Slot、跨 Session 批处理看板 | 在核心语义稳定后再扩展 |

## 1. 审核边界与证据等级

本报告严格区分“当前运行观察、源代码事实、推导和建议”：

- 【运行观察】来自 2026-08-17 实际启动 AF Web UI 和当前 Codex Desktop 后的真实截图。
- 【源代码事实】来自当前本地检出的 AF、Claude Code 和 DSH 源码。
- 【文档事实】来自仓库自带文档；不自动等同于已编译、已测试或已在生产运行。
- 【推导】由多处证据组合得到，仍需通过实现和测试确认。
- 【建议】目标设计，不代表当前已经存在。

### 1.1 当前代码快照

| 项目 | 当前快照 | 本次使用方式 |
|---|---|---|
| AF / Taskflow | phase-4，0c38ce4a8333c505371fd4b47afa58abe7e69c91 | 源码、SQLite、文档和真实 Web UI |
| DeepSeek Harness | master，47f943859bef60e4160492346772ded9b24f765a | 源码与文档；未安装 node_modules，未宣称完成实时界面验证 |
| Claude Code source | main，3da94d5e5f2b99c9d82b0d8f09448b04775cd41f | 源码级交互模式分析 |
| Codex Desktop | 2026-08-17 当前运行实例 | 真实界面观察；不推断其内部实现 |

AF 工作树在审核时已有用户未提交修改。本报告只读取这些修改并输出本文件，不修改 Taskflow 源码。

### 1.2 AF 真实界面证据

本次启动了 AF 当前 Web UI 的 demo-state：

- 主会话截图：/tmp/taskflow-gui-audit-web.png
- Operations 截图：/tmp/taskflow-gui-audit-operations.png
- 当前 Codex Desktop 截图：/tmp/codex-shot-2026-08-17_18-59-32.png

截图仅作为本次审核证据，位于临时目录，不属于长期交付物。对应 SHA-256：

| 截图 | SHA-256 |
|---|---|
| AF 主会话 | 4d1e7894f67cd094591304371d98a84bda7cb2821dd12586a581a76a19760ba9 |
| AF Operations | 2c2cc009d99f5806a2c8e63952e4514be60225c16ab9100e5b0fc86ea3e9edf8 |
| Codex Desktop | c4304623defd00a01a350bf6de7474701b11596587ed89e38882abc15bb86199 |

## 2. AF 当前界面的事实审计

### 2.1 Legacy Session 仍是固定连接名；正式 API 已具备产品对象

> 2026-08-18 实现更新：下列原始截图和逐行事实记录保留为历史审计基线。
> 当前正式 `web/` Workbench 已接入版本化 Session/Run API；legacy
> `examples/web_ui_static` 仍只接受 `default`，不应被解释为产品级多 Session UI。

【源代码事实】

- ~~agent_framework/examples/web_ui_demo.cpp:53 定义进程级 g_agent_busy。~~ 已移除；legacy
  adapter 改用 `(session_id, run_id)` 租约注册表。
- 同文件约 312 行把 WebConnectionInfo.session_id 固定为 default。
- 同文件约 337 行把 interaction_context.conversation_id 固定为 default。
- /ui/sse 只接受 default，其他 Session 返回 404。
- agent_framework/examples/web_ui_static/app.js:490 固定连接 /ui/sse?session=default。
- index.html 左侧只渲染一个静态 default Session。

【判断】

当前 Session 没有以下产品级能力：

- 列表与分页；
- 标题、状态、所属组织/项目/工作区；
- 新建、打开、重命名、置顶；
- 归档、移入回收站、恢复、永久清除；
- 成员与权限；
- 最新 Run、未读事件、待审批、失败状态；
- 数据、Artifact、记忆快照的关联；
- 并发修订号与审计记录。

因此，当前 UI 上的 Session 更接近一个 SSE 连接标签，而不是用户可管理的工作对象。

### 2.2 Legacy 单 Session 门闩与正式多 Session Run Supervisor

【源代码事实】

2026-08-18 后，`/ui/run` 不再使用进程级 `g_agent_busy`。legacy adapter
按 Session 保存对话状态，并以 Run 身份持有取消控制；同一 legacy Session 的并发
Run 被拒绝，错误或异常退出会自动释放租约。正式多 Session 并行、lease/fencing、
重启接管和命令队列由 `SessionRunWorker`/`SQLiteRunSupervisor` 承担。

【判断】

不能通过复制 legacy 前端 Session 行来获得产品级并行能力；该入口仍只服务
`default`。事件层已补齐 session 定向 token/thinking/aux/final/error 分发并有负向
隔离测试，但 Memory、Tool、Artifact 的真实浏览器端到端隔离仍需 Live 认证。

正确做法不是把 bool 改成 map<bool>，而是引入：

- Session Catalog：管理产品对象；
- Run Supervisor：管理每个 Session 的 Run 生命周期；
- Scheduler：处理组织、用户、项目和 Provider 的配额与公平队列；
- Lease/Fencing：保证恢复、接管和重试不会重复执行外部副作用；
- Multiplex Event Gateway：按授权订阅多个 Session 的事件。

### 2.3 页面展示了大量“像按钮”的信息，但没有闭环

【运行观察 + 源代码事实】

当前主界面输入区显示 Attach、Files、Expression、Draw、Skill、MCP，但 index.html 中它们只是 span。左侧 Capabilities 与 Skill Status 主要是状态展示，不是管理入口。命令面板提供的动作很少，右侧 Tool Activity 即使没有活动也永久占据宽度。

【判断】

这违反了标准工作台的可供性原则：交互元素应当可操作；不可操作的信息应以普通说明呈现；未实现功能应隐藏，或以禁用态明确说明原因与启用路径。

建议建立服务端下发的 Capability Manifest。前端只有在服务端声明动作存在且当前用户有权执行时，才渲染可操作控件。

### 2.4 Operations 很强，但位置不对

【运行观察】

Operations 页面已经能展示 response、pipeline、verified 三轴状态、Task 命令、Agent Template、Skill pin、Runner、证据和 Artifact。这是 AF 相比通用聊天界面的差异化价值。

问题不是信息无用，而是：

- 它与会话正文平级争夺主工作区；
- 卡片和表格同时常驻，认知负担过高；
- 同一信息在状态栏、右栏和 Operations 中重复；
- 缺少“从当前回答跳到对应 Run/Task/Artifact”的对象导航；
- 权限、配置入口与只读状态混在一起。

建议保留 Operations 语义，但改为三层呈现：

1. 会话内紧凑 Run Strip：只显示正在做什么、进度、等待原因和下一步。
2. 右侧 Activity/Plan/Evidence 抽屉：展示当前 Run 的上下文细节。
3. 独立 Operations 页面：跨 Session 查询、筛选、批量管理和审计。

### 2.5 数据库已有 Conversation 运行数据，但没有产品目录

【源代码事实】

当前 web_ui_demo-conversation.sqlite3 已有 conversation messages、turns、events、inputs、tasks 等运行记录，主要以 tenant、conversation 为键。现有 agent/session/SessionStore 保存 runtime checkpoint、tool commits 和 child tasks。

【判断】

现有 SessionStore 是恢复执行用的 checkpoint store，不应直接承担产品 Session 目录。两者名称相同但生命周期不同：

- 产品 Session：用户创建、命名、共享、归档、搜索和删除。
- Runtime checkpoint：为某个 Run 或线程恢复执行。
- Conversation：消息与 Turn 的逻辑容器。
- Task：目标与闭环状态。
- Run：一次具体执行尝试。

建议新增 ProductSessionCatalog，并考虑把现有 SessionStore 在 API/文档层明确命名为 RuntimeCheckpointStore，避免长期混淆。

## 3. 三个参考产品真正值得借鉴的部分

### 3.1 Claude Code：主界面克制，复杂能力进入聚焦流程

【源代码事实】

Claude Code 的 ResumeConversation/LogSelector 支持历史会话选择、检索、跨项目查看、恢复和 fork。设置、后台任务、恢复和配置通常进入独立的选择器、对话框或命令流程。CLI 入口还提供后台 Session 的查看、日志、附着和终止语义。

【应吸收】

- 会话恢复、检索、fork 是一级能力；
- 主会话界面保持克制；
- 设置、权限、后台任务和 Skill 选择用聚焦流程承载；
- 键盘优先：搜索、命令面板、快捷切换；
- 后台执行可见，但不把完整调度器铺在主屏。

【不应照搬】

Claude Code 是终端优先的个人开发 Agent，不是多组织、多用户管理后台。AF 不能把 CLI 的本地 Session 语义直接当作企业协作模型。

### 3.2 Codex Desktop：工作区分组的任务列表与单焦点工作区

【运行观察】

当前 Codex Desktop 使用左侧任务导航、New Task、搜索和工作区分组；中央聚焦一个任务；计划和进度嵌入当前任务；设置与用户入口放在侧栏底部。多个任务可以存在并在后台推进，但界面不会常驻一个空的全局 Inspector。

【应吸收】

- 左侧 Session/Task 列表是主要导航；
- 按项目或工作区分组；
- 中央一次聚焦一个 Session；
- 后台运行状态压缩到列表徽标和当前任务进度；
- 用户、设置位于稳定且可预期的位置；
- 新建任务入口明显，搜索始终可达。

【不应误推】

这里只把当前运行界面作为交互参考，不推断 Codex Desktop 的内部数据库、权限或调度实现。

### 3.3 DeepSeek Harness：模块化 Session/Workspace 浏览器

【源代码事实】

DSH 客户端使用 ConnectionController → SessionManager → Session 的业务对象层；React UI 通过 Cordis Slot 组合 sidebar、workspace、settings 等模块。WorkspaceBrowser 支持搜索、分组、排序、新建、打开、fork、重命名、归档、工作区管理和运行状态标记。凭据页面只返回状态，不把 secret 值回传前端。

【应吸收】

- Session Manager 独立于 React 视图；
- Workspace/Session 浏览器是模块，而不是散落在页面里的 DOM 操作；
- 运行中、待审批、计划、问题等状态直接映射到 Session 行；
- 设置按模块贡献，凭据只显示 configured/source/writable/test status；
- UI Slot 为后续 Agent、Skill、Workflow 插件贡献入口。

【必须补足】

DSH 当前 Web transport 文档明确区分“可达性策略”和“身份认证”，不能把它视为多用户安全实现。其当前 Stage 也主要聚焦一个已选 Session，不等于多窗格并行工作台。AF 应吸收其模块化方法，而不是复制其安全边界。

### 3.4 综合取舍

| 能力 | Claude Code | Codex Desktop | DSH | AF 目标 |
|---|---|---|---|---|
| 主界面 | 终端会话，聚焦 | 单任务聚焦 | 单 Session Stage | 单 Session 聚焦 |
| Session 导航 | resume/search/fork | 左侧任务列表 | Workspace/Session Browser | 左侧组织/项目/工作区/Session |
| 后台并行 | 后台任务语义 | 多任务后台存在 | Session 状态可见 | 多 Session 调度 + 可恢复 Run |
| 多用户身份 | 非核心 | 本次不推断 | 当前 Web 不提供认证层 | OIDC、RBAC、审计、协作冲突语义 |
| 复杂状态 | 对话框/命令 | 任务内进度 | 模块化面板 | 动态抽屉 + 独立 Operations |
| Agent/Skill | 模型/指令驱动 | 任务驱动 | 插件模块 | 模型驱动、指令驱动、Workflow 节点 |
| 完成证明 | 工具结果/会话 | 计划与任务结果 | Harness 事件 | AF Closure + Receipt + Evidence |

## 4. 目标产品对象模型

### 4.1 Session 的正式定义

建议将 Product Session 定义为：

> 一个有稳定身份、所有者、成员、项目上下文、对话历史、数据引用、Agent/Skill 配置、记忆策略和多个 Run 的长期工作容器。

Session 不应等同于：

- 一条 SSE 连接；
- 一次模型请求；
- 一个 Task；
- 一个 Run；
- 一个 AgentThread checkpoint；
- 一份 Memory；
- 一个浏览器页签。

### 4.2 关系模型

~~~mermaid
erDiagram
    ORGANIZATION ||--o{ ORG_MEMBERSHIP : has
    USER ||--o{ ORG_MEMBERSHIP : joins
    ORGANIZATION ||--o{ PROJECT : owns
    PROJECT ||--o{ WORKSPACE : contains
    WORKSPACE ||--o{ SESSION : groups
    USER ||--o{ SESSION_MEMBERSHIP : participates
    SESSION ||--o{ SESSION_MEMBERSHIP : shares
    SESSION ||--o{ CONVERSATION : contains
    SESSION ||--o{ TASK : defines
    SESSION ||--o{ RUN : executes
    RUN ||--o{ TURN : advances
    RUN ||--o{ EVENT : emits
    RUN ||--o{ ARTIFACT : produces
    RUN ||--o{ APPROVAL : requests
    SESSION ||--o{ DATA_REFERENCE : links
    SESSION ||--o{ MEMORY_SNAPSHOT_REF : pins
    USER ||--o{ AUDIT_EVENT : acts
~~~

关键约束：

- 一个用户可属于多个组织。
- 一个 Session 归属一个组织、项目和工作区，但可有多个成员。
- SessionMembership 至少支持 owner、editor、reviewer、viewer。
- Session 关联数据使用引用和权限快照，不默认复制原始数据。
- Run 固定引用创建时的 Agent Template、Skill bundle、Workflow、模型、权限和记忆选择快照。
- Memory Record 保持独立生命周期；Session 只记录使用过的 Memory Snapshot Ref。

### 4.3 统一身份上下文

AF 已经分别存在：

- ConversationIdentity：tenant_id、conversation_id；
- ContractIdentity：tenant、organization、principal、project、task、run、plan、memory；
- MemoryScope：tenant、organization、principal、agent、project、workspace、task、run、turn；
- TaskCommandPrincipal：actor、conversation、scopes、authenticated。

这些是可复用基础，但目前是碎片化身份。建议定义唯一的 RuntimeSubject：

| 字段 | 含义 |
|---|---|
| tenant_id | 部署或租户隔离边界 |
| organization_id | 公司/组织 |
| principal_id | 当前用户或服务身份 |
| project_id | 项目 |
| workspace_id | 工作区 |
| session_id | 产品 Session |
| conversation_id | 消息容器 |
| task_id | 目标任务 |
| run_id | 一次执行 |
| turn_id | 一轮交互 |
| agent_id | 当前 Agent/角色 |
| auth_revision | 身份与权限版本 |

所有 API、事件、工具调用、审批、记忆检索和审计都传递同一个 RuntimeSubject；禁止在下游自行从字符串或环境变量重新推断用户。

## 5. 推荐的目标界面架构

### 5.1 桌面宽屏布局

~~~mermaid
flowchart LR
    L["左栏：组织 / 项目 / 工作区<br/>新建、搜索、Session 列表<br/>用户、设置、管理"]
    C["中央：Session Header<br/>会话与工作成果<br/>按需 Run Strip<br/>Composer"]
    R["右侧按需抽屉<br/>Activity / Plan / Files<br/>Agent / Skills / Memory<br/>Approval / Evidence"]
    L --> C
    C -. "用户打开上下文" .-> R
~~~

响应式规则：

- 大于等于 1280 px：左栏固定可折叠，右栏按需打开。
- 768–1279 px：左栏抽屉化，右栏覆盖式打开。
- 小于 768 px：单列；Session 列表、工作区和上下文面板使用路由切换。
- 右栏没有内容时不保留占位宽度。

### 5.2 左侧 Session 导航

左栏承担管理和切换，不再展示静态 Capability 清单。

顶部：

- 组织切换器；
- 项目/工作区面包屑或组合选择器；
- New Session 主按钮；
- 全局搜索，支持标题、内容、成员、标签、状态、Artifact 和时间范围。

Session 行：

- 标题；
- 所属工作区；
- 最新更新时间；
- 运行状态点：running、queued、waiting approval、blocked、failed、completed；
- 未读事件/协作更新；
- 成员头像或共享标记；
- 更多菜单：重命名、置顶、移动、分享、fork、归档、移入回收站。

分组：

- 置顶；
- 最近；
- 运行中；
- 待我审批；
- 已归档；
- 回收站。

Session 列表必须支持：

- 服务端分页和游标；
- 防抖搜索；
- 状态、成员、Agent Template、标签和日期过滤；
- 最近访问排序、更新时间排序、自定义排序；
- 键盘上下选择、Enter 打开、快捷菜单；
- URL 稳定定位，刷新后仍打开同一 Session。

### 5.3 中央会话/工作区

Header 只保留：

- Session 标题与面包屑；
- 当前状态；
- 当前成员/分享入口；
- 当前 Agent Template 或业务模式；
- 打开上下文抽屉；
- More 菜单。

正文采用对象化消息：

- 用户输入；
- Agent 回答；
- Tool Call；
- Approval；
- Plan/Task 更新；
- Artifact；
- System/Policy 事件；
- 协作者事件。

复杂内容默认摘要，点击后展开。每个 Agent 回答应能跳转到：

- 对应 turn_id；
- 对应 run_id；
- 使用的模型、Agent Template 和 Skill pin；
- Tool receipt；
- Artifact；
- Closure/Evidence 结论。

### 5.4 Run Strip

Run Strip 代替当前常驻的大块 Operations：

| 状态 | 主界面显示 |
|---|---|
| Running | 当前步骤、耗时、进度、Stop、查看 Activity |
| Queued | 队列位置、等待原因、取消 |
| Waiting approval | 审批对象、风险等级、Approve/Reject |
| Blocked | 阻塞原因、需要谁做什么、Retry/Change input |
| Completed | 结果摘要、证据等级、查看 Artifact |
| Failed | 失败类别、是否可重试、查看日志 |

不得用“模型说完成了”作为 Completed。状态必须来自 TaskClosureController、Runner receipt 和持久化事件的组合判断。

### 5.5 右侧上下文抽屉

抽屉由用户或当前事件打开，支持固定和关闭：

1. Activity：实时工具、子 Agent、Workflow 节点、耗时、重试。
2. Plan：目标、步骤、依赖、完成门槛、阻塞。
3. Files/Artifacts：输入数据、生成文件、差异、预览、下载。
4. Agent/Skills：当前模板、业务模式、Skill pin、Runner、权限。
5. Memory：本轮选中/排除的记忆、来源、权限、时效和冲突。
6. Approvals：待审批、历史决策、审批人和理由。
7. Evidence：Closure、Receipt、验证命令、测试结果和复现实例。

抽屉内容按当前 Session/Run 过滤，不做全局信息墙。跨 Session 运维进入独立页面。

### 5.6 独立管理页面

| 页面 | 职责 |
|---|---|
| Sessions | 跨项目查询、批量归档、转移、回收站和恢复 |
| Runs & Approvals | 跨 Session 运行、队列、阻塞、审批和资源占用 |
| Agents & Skills | Agent Template、Skill bundle、Runner、版本、测试和发布 |
| Workflows | Workflow 节点编排、输入输出契约和运行历史 |
| Memory | 公司、用户、项目、工作区记忆治理 |
| Data & Artifacts | 数据引用、权限、血缘、保留策略 |
| Settings | 组织、用户、项目、工作区、Session 默认值 |
| Admin & Audit | 用户、角色、SSO、配额、审计、保留与合规 |

## 6. Session 全生命周期设计

### 6.1 查询

GET /api/v1/sessions 支持：

- organization_id、project_id、workspace_id；
- member_id、owner_id；
- state、run_state、approval_state；
- agent_template_id、skill_id、tag；
- created_from/to、updated_from/to；
- query 全文检索；
- cursor、limit、sort。

搜索结果必须经过权限裁剪，不能先返回 ID 再由前端隐藏。

### 6.2 新建

New Session 使用短向导，默认值足够合理时应能一步创建：

1. 组织、项目、工作区；
2. Agent Template；
3. 业务模式；
4. 模型与权限配置；
5. 记忆策略；
6. 共享成员；
7. 初始任务或输入。

业务模式明确支持：

- Model-Driven：模型根据目标自主选择多个 Skill。
- Directive-Driven：Agent 指令明确规定 Skill 顺序、约束和退出条件。
- Workflow-Node：Agent/Skill 作为确定性 Workflow 节点嵌入。

创建结果是 Draft Session。只有收到第一条输入或显式 Run 命令后才创建 Run，避免空 Session 立即消耗资源。

### 6.3 打开

打开 Session 的顺序：

1. 校验当前用户 membership 和有效权限；
2. 返回 Session metadata、最新 revision、可执行能力清单；
3. 分页加载最近消息；
4. 订阅该 Session 的事件流；
5. 加载当前 Run、待审批和未读状态；
6. 按需加载 Artifact、Activity 和 Memory 明细。

禁止打开页面时一次性读取全部历史事件和全部 Artifact。

### 6.4 归档、删除与恢复

用户提出的 Delete 应实现为安全的三段式生命周期：

| 动作 | 语义 | 可恢复 |
|---|---|---|
| Archive | 从默认列表隐藏，运行记录与数据保留 | 是 |
| Move to Trash | 设置 deleted_at，终止或接管活动 Run 后进入保留期 | 是 |
| Purge | 依据保留、审计、Legal Hold 和 Artifact 策略清理 | 否 |

删除正在运行的 Session 时：

1. UI 显示活动 Run 与外部副作用风险；
2. 用户选择先 Cancel、Detach 后删除，或只归档；
3. 后端写入 DeleteRequested 事件；
4. Run Supervisor 停止接受新命令；
5. 对未决工具副作用执行 reconcile；
6. 完成后软删除；
7. 失败则标记 DeleteBlocked，并给出可操作原因。

永久清除只对 owner/admin 开放，并要求二次确认。审计记录、合规保留和不可删除的外部 Artifact 必须明确说明。

### 6.5 Fork 与移动

- Fork 创建新的 Session ID，可选择继承消息范围、Artifact 引用、Agent/Skill pin 和记忆快照。
- Fork 不继承正在运行的 lease、未决工具提交或审批票据。
- Move 允许在有权限的项目/工作区之间移动，但要重新计算策略、成员和数据权限。
- 活动 Run 默认不可移动；应等待结束或创建新 Session/Fork。

## 7. 单用户多 Session 并行

### 7.1 并行语义

建议默认规则：

- 同一用户可以并行运行多个 Session。
- 同一 Session 同一时刻默认只允许一个顶层 active Run。
- 一个 Run 内部可以并行执行无冲突的工具、子 Agent 和 Workflow 节点。
- 用户在活动 Run 期间的新输入必须显式归类为 steer 当前 Run、queue next turn、fork new Session 或 start separate Run。

这比简单允许同一 Session 多线程写入更可控，也更容易复现。

### 7.2 调度流程

~~~mermaid
flowchart TD
    A["用户向 Session 提交命令"] --> B["校验身份、角色和 Session revision"]
    B --> C["写入幂等 Command Event"]
    C --> D{"该 Session 有 active Run?"}
    D -- "否" --> E["创建 Run 并进入 Scheduler"]
    D -- "是：steer" --> F["写入当前 Run 控制通道"]
    D -- "是：queue" --> G["加入 Session 下一 Turn 队列"]
    D -- "是：fork" --> H["创建新 Session 与新 Run"]
    E --> I["组织/用户/项目/Provider 配额判断"]
    I -- "可运行" --> J["获取 Session Executor Lease"]
    I -- "等待" --> K["持久化 queued 状态与原因"]
    J --> L["Runner 执行并发安全的 Execution IR"]
    L --> M["事件、Receipt、Artifact 持久化"]
    M --> N["TaskClosureController 判定"]
    N --> O["释放 Lease，唤醒下一命令"]
~~~

### 7.3 必须替换的全局状态

g_agent_busy 应被以下对象取代：

- RunSupervisor[session_id]；
- active_run_id；
- command_queue；
- cancellation_source；
- executor_lease；
- state_revision；
- subscriber set；
- durable event cursor。

进程级仍可保留全局容量计数器，但它只能表示资源额度，不能表示业务 Session 是否忙。

### 7.4 公平与限额

Scheduler 至少考虑：

- organization 并发上限；
- principal 并发上限；
- project 优先级；
- Provider/模型限流；
- Tool/MCP 连接上限；
- CPU、GPU、内存和本地进程配额；
- 长任务与交互任务的公平性；
- 审批等待不占执行槽；
- detached/background Run 的优先级。

推荐加权公平队列：

1. 按组织分桶；
2. 组织内按用户轮转；
3. 用户内交互 Session 优先于后台批处理；
4. 长任务使用 aging 防止永久饥饿；
5. 资源不可用时记录 machine-readable wait_reason。

### 7.5 长任务与恢复

Session 并行不能破坏 AF 现有长任务语义：

- attach 只附着到已有 effect，不得静默重启；
- reconcile 只确认外部世界事实，不得把不确定状态当作成功；
- Retry 必须复用或更新 idempotency key；
- lease 接管使用 fencing token，旧执行器的后续提交必须被拒绝；
- 前端断线不终止后台 Run；
- Run 的完成由 Closure 决定，不由 WebSocket 断开或 UI 动画决定。

## 8. 多用户协作模型

### 8.1 多用户不是“给表加 user_id”

真正的协作需要同时解决：

- 身份认证；
- 组织与项目成员关系；
- 对 Session、数据、记忆、Agent、Skill、工具和审批的授权；
- 同时编辑与同时输入的冲突；
- 活动 Run 的唯一执行权；
- 每个动作的用户归属；
- 撤销、恢复和审计；
- 实时 presence 与断线重连；
- 跨租户隔离。

只让多个浏览器连接同一 SQLite 文件，不能称为多用户协作。

### 8.2 Session 成员角色

| 角色 | 默认权限 |
|---|---|
| Owner | 管理 Session、成员、删除、转移、策略范围内的全部操作 |
| Editor | 对话、运行、修改 Session 配置、创建 Artifact |
| Reviewer | 查看全部内容、审批被授权的动作、评论 |
| Viewer | 只读会话、结果和允许查看的 Artifact |

组织和项目角色可授予默认上限，但 Session 角色不能突破上级策略。

### 8.3 同一 Session 的同时输入

建议使用“单顶层 Run + 多人命令日志”模型：

1. 每个输入先成为带 actor_id、client_command_id、base_revision 的 Command。
2. 若无 active Run，首个合法命令创建 Run。
3. 若已有 active Run，UI 要求选择：
   - Steer current：影响当前执行；
   - Queue next：排队为下一 Turn；
   - Comment only：只记录协作意见；
   - Fork：创建独立 Session。
4. 两个用户同时修改标题、成员或配置时使用 If-Match/revision。
5. 版本冲突返回 409，并附最新对象和可重放动作，不做最后写入者静默覆盖。

### 8.4 审批冲突

Approval 必须声明策略：

- first-valid-decision；
- owner-only；
- any-of-role；
- quorum；
- separation-of-duty。

每个决策包含 approval_revision。已完成审批再收到旧 revision 的请求时返回 stale，而不是重复执行工具。

### 8.5 Presence 与持久事件

- 光标、正在输入、当前查看页面属于短期 presence，可放内存或 Redis。
- 消息、命令、审批、配置修改、Agent/Skill 变更和工具结果属于持久事件。
- Presence 丢失不影响执行正确性。
- 事件重连以 last_event_id 恢复，客户端按 event_id 去重。

SQLite 可支撑单节点起步；多节点生产部署应将 presence/pub-sub 与持久业务库分离，并明确数据库的一致性与租户隔离策略。

## 9. 公司、用户、项目、工作区与记忆设置

### 9.1 两条继承链，不使用一条万能覆盖链

建议拆成：

#### Policy Plane

System → Organization → Project → Workspace

- 安全、合规、数据边界、模型白名单、Tool 风险、保留期和网络权限。
- 下级只能收紧，不能放宽。

#### Preference Plane

System default → Organization default → User → Session

- 主题、语言、默认 Agent Template、默认模型、通知、布局、常用 Skill。
- 只能覆盖非政策项。

Run 创建时生成不可变 EffectiveConfigSnapshot。之后修改默认设置，不改变正在运行或已完成 Run 的证据边界。

### 9.2 设置 UI 必须显示来源

每个配置项显示：

- Effective value；
- 来源层级；
- 是否被上级锁定；
- 当前 revision；
- 修改权限；
- 生效时机：立即、下一 Turn、下一 Run、重启后；
- 最近修改人和时间；
- 恢复继承按钮。

这比简单做五套相似表单更重要。

### 9.3 各层设置范围

| 层级 | 典型内容 |
|---|---|
| Organization | SSO、成员、RBAC、模型白名单、数据地域、审计、预算、全局记忆政策 |
| User | 个人默认模型、通知、主题、语言、常用 Agent/Skill、个人记忆 |
| Project | 数据源、知识库、默认 Template、Tool allowlist、项目记忆、预算 |
| Workspace | 路径、仓库、环境、Runner、MCP、工作区指令和记忆 |
| Session | 参与者、业务模式、本 Session Template/Skill、共享范围、记忆选择策略 |
| Run | 不可变配置快照、权限快照、Skill pin、模型、资源和幂等策略 |

### 9.4 记忆管理

记忆页不应只是开关，应支持：

- Scope：公司、用户、项目、工作区、Session/Task；
- Authority：candidate、verified、authoritative；
- 来源与 provenance；
- ACL 与敏感级别；
- freshness/过期；
- 冲突组与 superseded 关系；
- 允许/禁止被哪些 Agent 或 Skill 使用；
- 本轮选中、排除和截断原因；
- 删除、tombstone、恢复和合规保留；
- 引用该记忆的 Run 与结果。

Session 页面只展示“本 Turn/Run 实际使用的记忆快照”，不复制全部记忆。这样可以回答：

- 为什么模型看到了这条信息？
- 为什么某条公司记忆没有被选中？
- 当时使用的是哪个 revision？
- 修改记忆后，旧结果是否仍可复现？

### 9.5 凭据与外部授权

学习 DSH 的原则：Secret 值不返回前端。UI 只显示：

- configured；
- source；
- writable；
- last tested；
- expires_at；
- scopes；
- rotation required。

凭据应由 Credential Broker 解析，Run 只获得短期引用或下游令牌，不把明文写入 Session event、日志、Artifact 或记忆。

## 10. Agent、Skill 与 Workflow 在 GUI 中的统筹

### 10.1 Agent Template 是 Session 创建的核心配置

Template 至少包含：

- 角色与系统指令；
- 业务模式；
- 可用模型及路由；
- Skill bundle 与版本 pin；
- Tool/MCP allowlist；
- 权限与审批策略；
- Memory policy；
- Runner 类型；
- 最大迭代、预算和完成门槛；
- 输出/Artifact 契约；
- Workflow 嵌入点。

用户在 Session 中看到的是 Template 的有效快照，而不是仓库目录里的一组松散 YAML。

### 10.2 三种业务模式的交互差异

| 模式 | GUI 重点 | 执行控制 |
|---|---|---|
| Model-Driven | 展示模型为何选择 Skill、候选与最终选择 | SkillCompiler + Policy + Runner |
| Directive-Driven | 展示指令规定的步骤、顺序、退出条件 | Agent 指令编译为 Execution IR |
| Workflow-Node | 展示节点输入输出、上游下游和重试 | Workflow 调度器拥有顶层控制 |

同一 Skill 的运行记录格式应统一，避免三种模式形成三套互不兼容的日志。

### 10.3 Agent & Skills 管理中心

列表页：

- 名称、版本、状态、所有者、适用 Scope；
- 发布渠道：draft、staging、production、deprecated；
- 最近测试、成功率、平均耗时；
- 权限需求、额外 Key、MCP 依赖；
- 被哪些 Template、Workflow、Session 引用。

详情页：

- 指令、输入输出 Schema；
- 依赖 Skill/Tool；
- Runner；
- 权限和审批；
- 测试用例；
- 版本差异；
- 发布与回滚；
- 运行证据。

### 10.4 动态 UI Capability

服务端下发 CapabilityManifest：

| 字段 | 示例 |
|---|---|
| action_id | session.attach_file |
| route/method | POST /api/v1/sessions/:id/data |
| required_scope | session:data:write |
| enabled | true/false |
| reason | MCP disabled / no permission / run active |
| expected_revision | 42 |
| ui_hint | toolbar / menu / approval |

前端不得自行根据一个布尔状态猜测权限。当前 Attach、Files、Expression、Draw、Skill、MCP 等静态文字要么接入 Manifest 并完成闭环，要么移除。

## 11. 后端 API 与实时通道

### 11.1 建议 API

身份与目录：

- GET /api/v1/me
- GET /api/v1/organizations
- GET /api/v1/projects
- GET /api/v1/workspaces

Session：

- GET /api/v1/sessions
- POST /api/v1/sessions
- GET /api/v1/sessions/:session_id
- PATCH /api/v1/sessions/:session_id
- DELETE /api/v1/sessions/:session_id
- POST /api/v1/sessions/:session_id/restore
- POST /api/v1/sessions/:session_id/purge
- GET/POST /api/v1/sessions/:session_id/members
- GET/POST /api/v1/sessions/:session_id/data

运行：

- GET/POST /api/v1/sessions/:session_id/runs
- GET /api/v1/runs/:run_id
- POST /api/v1/runs/:run_id/commands
- GET /api/v1/runs/:run_id/events
- GET /api/v1/runs/:run_id/artifacts
- GET/POST /api/v1/runs/:run_id/approvals

设置与能力：

- GET /api/v1/settings/effective
- PATCH /api/v1/settings/:scope/:scope_id
- GET /api/v1/capabilities
- GET /api/v1/credentials/status
- POST /api/v1/credentials/:provider/test

### 11.2 并发协议

- POST 命令要求 Idempotency-Key。
- PATCH/DELETE 要求 If-Match 或 expected_revision。
- 事件具有单调 event_sequence 和稳定 event_id。
- 创建 Run 返回 202 + run_id；不把长执行绑在 HTTP 请求生命周期。
- 查询返回状态，而不是通过重复 POST 猜测是否执行成功。
- 工具副作用返回 durable receipt_id。

### 11.3 实时通道

建议主通道使用已在 AF 依赖中的 websocketpp，实现授权后的多 Session multiplex：

~~~json
{
  "type": "subscribe",
  "session_ids": ["ses_01", "ses_02"],
  "after": {"ses_01": 182, "ses_02": 31}
}
~~~

服务端逐项校验订阅权限；禁止因用户能连接 WebSocket 就允许查看任意 Session。

SSE 可保留为兼容回退，但不能继续固定 default。每个事件至少包含：

- event_id；
- event_sequence；
- organization_id；
- session_id；
- run_id；
- actor_id；
- event_type；
- occurred_at；
- payload schema version。

## 12. 数据模型与持久化

### 12.1 建议表

管理域：

- organizations
- users
- organization_memberships
- projects
- project_memberships
- workspaces
- sessions
- session_memberships
- session_tags

执行域：

- conversations
- messages
- tasks
- runs
- turns
- run_commands
- session_events
- runner_leases
- tool_effects
- effect_receipts
- approvals

数据与知识域：

- data_assets
- session_data_refs
- artifacts
- memory_records
- memory_snapshot_refs

治理域：

- settings
- effective_config_snapshots
- credential_refs
- audit_events
- retention_holds

### 12.2 Session 核心字段

| 字段 | 要求 |
|---|---|
| session_id | 稳定、不可复用 |
| tenant/org/project/workspace | 完整归属 |
| owner_principal_id | 创建者/所有者 |
| title/summary/tags | 可搜索 |
| state | draft/active/archived/trashed |
| visibility | private/project/invite-only |
| revision | 乐观并发 |
| latest_run_id | 快速列表投影 |
| latest_activity_at | 排序 |
| deleted_at/purge_after | 安全删除 |
| effective_config_snapshot_id | 当前默认快照引用 |
| created_by/updated_by | 审计 |

### 12.3 投影与事件

Session 列表不能每次聚合所有 message/event。建议：

- command/event log 作为事实源；
- SessionListProjection 保存标题、状态、最后活动、当前 Run、待审批和未读；
- 投影失败可从事件重建；
- UI 接收事件后局部更新，定期以服务端 revision 校正；
- Artifact 与大日志独立存储，只保存元数据和内容地址。

### 12.4 SQLite 与生产数据库

SQLite 适合本地单机、开发、桌面版和单节点原型。多用户服务版需要明确：

- 单写者瓶颈；
- 多实例 lease；
- 备份恢复；
- 加密与密钥管理；
- 数据保留；
- 全文检索；
- 租户隔离；
- 大 Artifact 外置。

建议保持 Repository 接口，使本地版使用 SQLite，服务版可切换 PostgreSQL。不要在第一版 UI 重构中同时强制迁移数据库，但 schema 必须从一开始包含完整身份与 revision。

## 13. 前端工程重构

### 13.1 推荐技术栈

当前 app.js 适合 Demo，不适合承载多对象、多路由、多权限和实时协作。建议新增正式 Web App：

- TypeScript + React + Vite；
- Router 管理 Session 与管理页面；
- TanStack Query 或等价方案管理服务端状态；
- Zustand/轻量 store 只管理布局、选择和本地草稿；
- CSS Modules + Design Tokens；
- 无障碍基础组件库，但视觉语言由 AF 自己定义；
- WebSocket client + SSE fallback；
- Schema 由后端契约生成或共享。

这里借鉴 DSH 的“业务对象层不依赖 React”：

~~~text
AuthContext
  └─ OrganizationManager
      └─ Project/WorkspaceManager
          └─ SessionManager
              ├─ SessionCatalog
              ├─ SessionConnection
              ├─ RunController
              ├─ ArtifactController
              └─ PresenceController
~~~

组件通过这些 Controller/Query 使用数据，不直接在 DOM handler 中拼接 fetch。

### 13.2 信息状态原则

每个页面统一处理：

- loading；
- empty；
- partial；
- stale/reconnecting；
- permission denied；
- deleted；
- offline；
- error with retry；
- schema incompatible。

不允许用空白面板表示“没有数据、未连接、无权限、功能未启用”四种不同状态。

### 13.3 标准界面规范

- 8 px 间距体系；
- 正文 14–16 px，状态文本不低于 12 px；
- 颜色不作为唯一状态信号；
- 键盘可完成新建、搜索、切换、发送、停止和打开命令面板；
- 清晰 focus ring；
- WCAG 2.2 AA 对比度；
- 中文/英文 i18n，不把字符串硬编码在组件；
- reduced motion；
- 长标题、长路径、超宽表格、数学公式和代码块都有明确溢出策略；
- 图标配 tooltip 和 accessible label；
- 危险动作与普通动作分组；
- 成功提示不遮挡输入；
- 不用持续动画表示长任务仍健康，健康来自 heartbeat/event。

### 13.4 UI Slot 扩展点

可借鉴 DSH Cordis Slot 思路，但 AF 不必引入 Cordis。定义受控扩展点：

- sidebar.section；
- session.header.action；
- composer.action；
- message.renderer；
- context.drawer.tab；
- settings.section；
- operations.panel；
- artifact.preview。

扩展必须声明：

- 所需 API capability；
- 所需 scope；
- 支持的 schema version；
- 加载失败降级；
- CSP/沙箱策略；
- 是否允许读取 Session 内容。

## 14. 身份、授权与额外 Key

### 14.1 人类用户认证

当前 A2A Bearer/API Key AuthGate 不能直接等同于 Web 用户身份系统。生产多用户版建议：

#### 首选

OIDC Authorization Code + PKCE，Web 使用 BFF/HttpOnly Secure SameSite Cookie。

需要：

- OIDC issuer；
- client_id；
- redirect_uri；
- logout_uri；
- session signing/encryption secret；
- 用户与组织映射规则。

#### 过渡方案

由可信反向代理完成 SSO，AF 只接受经过 mTLS 或受信网络注入的身份头。必须配置 trusted_proxy 和 header 签名/防伪，不能信任任意客户端提交的 X-User。

### 14.2 Key/授权矩阵

| 功能 | 是否额外 Key/授权 | 说明 |
|---|---|---|
| 本地 Session CRUD | 不需要外部 Key | 需要本地数据库写权限 |
| 多用户登录 | 需要 IdP 配置 | OIDC client 或可信企业代理 |
| LLM 调用 | 需要 Provider 凭据或订阅登录 | 沿用现有 Adapter/Credential Broker |
| MCP/外部 Tool | 视服务而定 | OAuth/API Key/本地权限应按 Tool 声明 |
| GitHub/GitLab | 通常需要 OAuth/PAT | 最小 scope，按用户或服务账号区分 |
| 云对象存储 | 服务端凭据/KMS | Artifact 外置时需要 |
| 邮件/Slack/Teams | OAuth/应用授权 | 涉及外部副作用，必须审批与审计 |
| 本地文件/进程 | OS 权限 | 不应伪装成 API Key 问题 |
| WebSocket/SSE | 复用登录会话 | 每次订阅仍做 Session 授权 |
| 数据库加密 | 生产建议 KMS/密钥 | 本地桌面版可采用 OS keychain |

### 14.3 授权检查位置

授权不是前端隐藏按钮。必须在以下位置重复执行：

1. API gateway；
2. Session command service；
3. Scheduler/Runner；
4. Tool/MCP invocation；
5. Data/Artifact fetch；
6. Memory retrieval；
7. Event subscription；
8. Approval decision；
9. Purge/export。

每次拒绝写入审计事件，但不得把敏感资源是否存在泄露给无权用户。

## 15. 迁移方案

### 15.1 Legacy default Session

建议迁移步骤：

1. 创建 Legacy organization/project/workspace。
2. 为当前本地用户创建 owner principal。
3. 将现有 tenant + conversation=default 回填为一个 Product Session。
4. 保留原 conversation_id，新增 session_id 映射。
5. 从现有消息、任务、事件计算 SessionListProjection。
6. 旧 /ui/* 接口暂时通过 adapter 访问新服务。
7. 新 UI 完成后关闭固定 default SSE。

### 15.2 双轨期

双轨期应短且可观测：

- 先 dual-read 验证新目录与旧 Conversation 一致；
- 写操作只经过新 Command API，再投影到兼容层；
- 不长期 dual-write 两套事实源；
- 每个迁移批次有数量、哈希、失败清单和回滚点；
- 旧 UI 明确标记 Legacy，不再新增功能。

### 15.3 删除迁移

历史记录没有 deleted_at 时默认 active。迁移后先只开放 Archive/Trash，待 retention、Artifact 引用和审计验证通过后再开放 Purge。

## 16. 分阶段升级计划

工期取决于团队规模。以下为顺序依赖，不是承诺日期；两到三个前后端/平台小组并行时，建议以 16–24 周为产品化窗口，单线顺序实施可能需要 25–40 周。

### Phase 0：语义冻结与假控件清理（1 周）

工作：

- 冻结 Session、Conversation、Task、Run、Turn、AgentThread 的定义。
- 建立当前 UI 控件到 API/Capability 的盘点表。
- 删除或降级无响应的 Attach、Files、Expression、Draw、Skill、MCP 伪入口。
- 定义状态机、错误码、审计事件和 Evidence 等级。

退出门槛：

- 一份评审通过的 Identity/Lifecycle RFC；
- 所有可见控件都有 action、detail、configuration 或明确只读语义；
- 不再新增 default Session 特例。

### Phase 1：产品 Session 与统一身份（2–4 周）

工作：

- ProductSessionCatalog；
- RuntimeSubject；
- Session CRUD、搜索、分页、revision；
- SessionMembership；
- Legacy default 回填；
- SessionListProjection。

退出门槛：

- API 可创建、查询、打开、重命名、归档、回收和恢复 Session；
- 所有新记录包含 org/project/workspace/principal/session；
- 跨项目越权测试全部拒绝；
- 旧数据迁移数量可核对。

### Phase 2：Run Supervisor 与多 Session 并行（3–5 周）

工作：

- 移除 g_agent_busy；
- 每 Session active Run 与命令队列；
- 全局 Scheduler 和配额；
- lease/fencing；
- WebSocket multiplex 与 SSE fallback；
- 后台运行、断线恢复和状态投影。

退出门槛：

- 同一用户三个 Session 同时运行，消息、工具、记忆和 Artifact 零串扰；
- 浏览器关闭后 Run 继续，重开后可恢复；
- 重复提交同一 Idempotency-Key 不重复副作用；
- 旧 executor 的 fenced commit 被拒绝。

### Phase 3：身份、组织与 RBAC（3–5 周）

工作：

- OIDC/BFF 或可信代理接入；
- Organization/Project/Workspace membership；
- Session roles；
- API、事件、工具、数据和记忆的统一授权；
- 审计和管理员页面基础。

退出门槛：

- 两个组织互不可见；
- viewer 无法通过直接 API 或 WebSocket 执行写操作；
- actor_id 可追溯到每条命令、审批和设置变更；
- Token 过期、撤权和断线重连行为可验证。

### Phase 4：新 Web Shell 与 Session 管理（3–4 周）

工作：

- TypeScript/React 正式前端；
- 三栏响应式 Shell；
- Session 列表、搜索、创建、打开、归档、回收站；
- URL 路由、快捷键和命令面板；
- Capability Manifest。

退出门槛：

- Session CRUD 全流程浏览器 E2E；
- 无空白永久右栏；
- 无伪按钮；
- 键盘可完成核心流程；
- 桌面与窄屏真实截图审查通过。

### Phase 5：会话工作区与渐进式 Operations（2–4 周）

工作：

- 对象化消息；
- Run Strip；
- Activity/Plan/Files/Agent/Memory/Approval/Evidence 抽屉；
- 跨 Session Operations 页面；
- 当前三轴状态和 TaskClosure 迁入新结构。

退出门槛：

- 从回答可定位到 Run、Receipt、Artifact 和 Closure；
- 主界面不展示与当前 Session 无关的运维信息；
- 断线、无权限、无数据和未启用有不同可理解状态。

### Phase 6：设置、凭据与记忆治理（3–5 周）

工作：

- Policy/Preference 两条继承链；
- EffectiveConfigSnapshot；
- 公司、用户、项目、工作区、Session 设置页；
- Credential Broker 状态页；
- Memory 治理与本轮选择解释。

退出门槛：

- UI 可解释每个有效值来源；
- 上级安全策略不能被下级放宽；
- 正在运行的 Run 不受默认值修改影响；
- Secret 永不通过读取 API 回显；
- Memory ACL、版本和选择理由可审计。

### Phase 7：实时协作（4–6 周）

工作：

- Presence；
- steer/queue/comment/fork；
- 乐观并发；
- 审批策略；
- 评论、@成员和通知；
- reconnect/replay。

退出门槛：

- 两个用户同时操作同一 Session 不丢命令、不静默覆盖；
- stale revision 可恢复；
- 审批重复提交不重复触发外部效果；
- Presence 丢失不改变持久执行状态。

### Phase 8：Agent/Skill/Workflow 管理中心（2–4 周）

工作：

- Template/Skill/Runner 目录；
- 三种业务模式；
- 版本 pin、测试、发布和回滚；
- Workflow 节点可视化；
- UI Slot 扩展。

退出门槛：

- 一个 Skill 可在三种模式下产生统一 Run/Event/Receipt；
- Session 可解释实际使用的 Template 与 Skill 版本；
- 未授权 Skill/Tool 在编译和运行两处都被拒绝；
- 发布版本可回滚且不改变历史 Run。

### Phase 9：生产认证与迁移收口（3–5 周）

工作：

- 性能、可访问性、国际化；
- 备份恢复、保留和 Purge；
- 多实例/数据库验证；
- ProviderLive、真实 MCP 和长任务恢复；
- 运维手册和迁移清理。

退出门槛：

- 零跨租户泄露；
- 零已知重复外部副作用；
- 零“模型自报完成但 Closure 未通过”的成功状态；
- 关键流程 WCAG 2.2 AA；
- 真实浏览器截图、E2E、故障注入和 ProviderLive 证据齐全；
- Legacy 写接口关闭。

## 17. 建议代码落点

以下是建议位置，不表示当前文件已经存在：

### C++ 领域与持久化

- agent_framework/include/agent/session/session_catalog.hpp
- agent_framework/include/agent/identity/runtime_subject.hpp
- agent_framework/include/agent/runtime/run_supervisor.hpp
- agent_framework/include/agent/runtime/scheduler.hpp
- agent_framework/include/agent/auth/authorization_service.hpp
- agent_framework/src/session/sqlite_session_catalog.cpp
- agent_framework/src/runtime/run_supervisor.cpp
- agent_framework/src/runtime/scheduler.cpp

### Web API

- agent_framework/include/agent/web/api/session_routes.hpp
- agent_framework/include/agent/web/api/run_routes.hpp
- agent_framework/include/agent/web/api/settings_routes.hpp
- agent_framework/include/agent/web/realtime/event_gateway.hpp
- agent_framework/src/web/api/*
- agent_framework/src/web/realtime/*

### 正式前端

- agent_framework/web/package.json
- agent_framework/web/src/app
- agent_framework/web/src/domain/session
- agent_framework/web/src/domain/run
- agent_framework/web/src/domain/settings
- agent_framework/web/src/domain/memory
- agent_framework/web/src/features/session-browser
- agent_framework/web/src/features/conversation
- agent_framework/web/src/features/context-drawer
- agent_framework/web/src/features/operations
- agent_framework/web/src/features/agent-skill-center

### 兼容层

- agent_framework/examples/web_ui_demo.cpp 只保留 demo bootstrap 和静态服务入口；
- 现有 /ui/bootstrap、/ui/run、/ui/sse 通过 adapter 映射到新 API；
- agent_framework/examples/web_ui_static 在新 UI 稳定后进入 deprecated。

## 18. 验证矩阵

### 18.1 功能

- Session 查询、新建、打开、重命名、fork、归档、回收、恢复、永久清除。
- 项目/工作区移动与权限重算。
- 多 Session 并行、后台继续、取消、重试和恢复。
- 同 Session steer、queue、comment、fork。
- Agent Template、Skill、Workflow 三种业务模式。
- 公司/用户/项目/工作区/Session 设置与有效值解释。
- Memory 选择、ACL、版本和 provenance。

### 18.2 一致性与故障注入

- POST 重放；
- WebSocket 重连与事件重复；
- Runner 崩溃；
- lease 超时与旧 worker 复活；
- Tool 执行成功但回执写入前崩溃；
- 数据库忙/磁盘满；
- Provider 限流；
- MCP 半连接；
- 审批过程中撤权；
- 删除过程中存在未决副作用。

### 18.3 安全

- 水平越权与跨租户 ID 猜测；
- WebSocket/SSE 越权订阅；
- viewer 直接调用命令 API；
- 记忆与 Artifact 泄露；
- 凭据回显与日志泄露；
- 反向代理身份头伪造；
- CSRF、XSS、CSP、上传文件；
- 删除、导出和 Purge 权限。

### 18.4 UI

- Chromium/Firefox/WebKit；
- 1440、1280、1024、768 和移动宽度；
- 键盘全流程；
- 屏幕阅读器标签；
- 对比度；
- 中文/英文长文本；
- Markdown、代码、公式、Mermaid、宽表；
- 空状态、错误、重连、无权限、旧 schema；
- 真实运行环境截图，不以 DOM 存在或 HTTP 200 代替视觉验证。

### 18.5 完成度分级

每个阶段都应区分：

1. Source：接口与实现存在。
2. Build：实际编入目标。
3. Offline Test：单元/集成测试通过。
4. Runtime：真实服务和浏览器交互通过。
5. ProviderLive：真实模型、MCP、外部副作用和恢复验证。
6. Production：监控、备份、权限、性能和故障演练通过。

不得用较低等级的证据宣称较高等级完成。

## 19. 主要风险与反模式

### 19.1 把 UI 改版先于 Session 语义

结果会是漂亮的多 Session 列表仍然调用固定 default SSE。必须先完成 Catalog、Identity 和 Run Supervisor 的最小垂直切片。

### 19.2 把 Session、Conversation、Task、Run 混成同一 ID

短期少写几张表，长期会导致 fork、重试、共享、删除、恢复和审计无法表达。必须保留稳定 Session 与多 Run 关系。

### 19.3 只在前端加权限

隐藏按钮不能防止直接 API、事件订阅或 Tool 调用。授权必须贯穿 API、Command、Runner、Tool、Data、Memory 和 Realtime。

### 19.4 认为 Session 并行等于线程并行

真正风险在共享状态和外部副作用。需要 lease、fencing、idempotency、receipt 和 reconcile，而不只是线程池。

### 19.5 把所有信息继续放在主屏

管理能力完整不等于所有卡片常驻。主屏要清爽，复杂信息通过对象导航、抽屉和独立管理页渐进披露。

### 19.6 设置继承无来源

如果用户只看到最终值，看不到组织策略、用户覆盖和 Run 快照，就无法解释为什么不能修改，也无法复现历史结果。

### 19.7 把“多人能打开”当作协作完成

多人协作的门槛是身份、角色、并发命令、审批冲突、事件重放和审计都闭环。

## 20. 推荐首个垂直切片

第一轮不要同时实现全部管理后台。建议用一个可验收的垂直切片证明架构：

1. 定义 Product Session、RuntimeSubject、Run 状态机。
2. 新增 SQLiteSessionCatalog 和最小 Session API。
3. 将固定 default 迁移成真实 Legacy Session。
4. 用 RunSupervisor[session_id] 替换 g_agent_busy。
5. 实现同一用户三个 Session 后台并行。
6. 新建最小 React Shell：Session 查询、新建、打开、归档、回收与恢复。
7. 将右侧 Tool Activity 改成按需抽屉。
8. 通过 Capability Manifest 只显示真实动作。
9. 把现有 response/pipeline/verified 与 TaskClosure 映射到 Run Strip。
10. 完成真实浏览器 E2E、断线恢复、幂等重放和串扰测试。

这个切片完成后，AF 才具备继续加入 OIDC、多用户协作、层级设置、记忆管理和 Agent/Skill 管理中心的稳定基础。

## 21. 最终建议

AF 应保留并强化自己的核心差异：Execution IR、Runner、Receipt/Reconcile、TaskClosure、多 Skill 编排和长任务恢复；界面层则吸收：

- Claude Code 的克制主界面、恢复/搜索/fork 和聚焦式复杂流程；
- Codex Desktop 的工作区分组 Session 列表、单焦点工作区和后台任务可见性；
- DSH 的 SessionManager、WorkspaceBrowser、模块化设置与凭据不回显。

最终不应做成另一个聊天壳，而应做成：

> 一个以 Session 为工作入口、以 Run 为执行边界、以组织和用户为权限主体、以数据与记忆引用为上下文、以 Agent/Skill/Workflow 为编排能力、以 Closure 和 Evidence 为完成证明的标准化 Agent 工作台。

界面重构的成功标准不是“看起来更像 Claude Code、Codex 或 DSH”，而是用户能在一个清爽界面里可靠完成：

- 找到和管理工作；
- 并行推进多个 Session；
- 与其他用户安全协作；
- 知道当前 Agent 正在做什么、为什么等待、是否真正完成；
- 配置公司、用户、项目、工作区与 Session；
- 解释每次运行使用了哪些数据、记忆、Agent、Skill、权限和版本；
- 在失败、断线、重启和长时间运行后继续收敛到可验证结果。

## 22. 2026-08-18 AF-NUI0–NUI7 原生界面对齐闭环

TUI 与 ImGui 已从各自维护的静态控制台布局迁移到同一套原生 Workbench
信息架构：Session rail、Run strip、Conversation/Understanding/Plan/Memory/Files/
Approval/Evidence 上下文导航、Tool Activity、Profile 和 System Settings。两端共用
`NativeWorkbenchController`，后者直接连接 `SessionRunApi` 与
`RuntimeSettingsStore`，因此会话重命名、回收、恢复、两阶段永久清除和设置变更均使用
服务端权威 revision/CAS，而不是仅修改本地 UI 状态。

原生 Session 切换现在同时更新 `RuntimeSubject` 和 harness-supported conversation
scope，并重置当前界面的瞬时 presentation/thread state，避免新任务继续写入上一个
Session 的 Conversation。System Settings 展示与正式 Web Workbench 相同的完整配置域：
Provider/Model、MCP、Skills、工具沙箱、工作目录、规划深度、记忆策略、
Assurance/Judge、日志、Observability、主题和语言；只读凭据状态不回显密钥，动态项与
需要重启的项明确区分。`tools/run_ui.sh` 新增 `--workbench-state-dir`，便于原生界面
使用隔离、持久的 Session/Settings 数据目录。

TUI 文本渲染不再依赖空格或 UTF-8 字节数断行。`wrap_terminal_text` 使用 FTXUI 的
glyph 分段和 terminal display width，覆盖连续中文、CJK 宽字符、emoji、组合字符、
显式换行与无断点长 URL；Conversation、Tool arguments/results、Operations、Memory、
LLM invocation、HITL 与设置值统一使用该路径。宽终端使用 Session/主上下文/Activity
三栏；中等和紧凑终端按可用列数降级，不再把固定三栏强塞入小窗口。

离线验证覆盖 presentation、Markdown assembler、operations、Session API、settings
CAS、原生控制器、TUI Unicode wrap、ImGui/TUI handler、launcher 与 rich renderer，
13/13 通过。真实运行验证在隔离 SQLite 目录和 Xvfb 显示服务器中启动了 Release
`tui_agent_demo` 与 `imgui_agent_demo`，并人工检查 Conversation 与 Settings 状态。

| 证据 | SHA-256 | 视觉检查 |
|---|---|---|
| `docs/evidence/screenshots/af-nui/imgui-conversation.png` | `3573e799ee4d9fc05c1835a3d43eec8d095b0a815871d3460f9c08e86d9789ea` | 通过；Session、Run、七类上下文、Markdown Conversation 与 Tool Activity 同屏且无裁切 |
| `docs/evidence/screenshots/af-nui/imgui-settings.png` | `ea19dfa07a8d2f956397e6076895a9d84ee3e21c5a9fd6cdb0361063528a91bc` | 通过；revision、授权版本、typed fields、只读凭据与 restart 标记可见 |
| `docs/evidence/screenshots/af-nui/tui-conversation.png` | `74886827c44628937dd8118754f65012ec3179c8e4882b573b64270039a629ed` | 通过；宽屏三栏、Session/身份入口和连续中文/长 URL 自动换行可见 |
| `docs/evidence/screenshots/af-nui/tui-settings.png` | `67e29b275d38736bb1cecb9962604a2d5e1c8fa8e633b188e5b061022424decd` | 通过；所有主要设置组在终端内可滚动查看，无水平溢出 |
| `docs/evidence/screenshots/af-nui/imgui-before-after.png` | `196efa2cd30465e5a586ef695ee38c4795c7be986a7e53275de6f8327f9b8c3f` | 基线/当前并排；从三个技术 tab 升级为 Session-first Workbench |
| `docs/evidence/screenshots/af-nui/tui-before-after.png` | `21eb4fce0fe18909c2bd9adbd949cad6f6d2c4df9fa21273c57011a470055abf` | 基线/当前并排；增加 Session、Run、上下文导航、身份/设置并验证 wrap |

本阶段闭合的是原生界面的管理入口、信息架构和单进程 durable state，不把截图或离线
测试外推为生产多用户认证。TUI/ImGui 仍是本地原生客户端；组织级 OIDC、跨节点
Session 并发、外部部署策略和 ProviderLive 认证继续沿用 Phase 4 对应证据等级。
