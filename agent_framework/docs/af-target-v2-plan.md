## 当前判断

  现有 AF 有较好的底座，但尚未达到 Claude Code、Codex、Cursor 常见 Skill 的直接兼容水平。

   能力                                当前状态    核心缺口
  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   ToolBus 调用、Schema、Hook、权限      约 75%    工具身份完全按名称匹配，没有 canonical capability/
                                                   alias 层
  ──────────────────────────────────  ──────────  ──────────────────────────────────────────────────────
   文件工具                              约 65%    只有 fs_read/fs_write/fs_replace/fs_search/fs_grep，
                                                   与 Read/Write/Edit/Glob/Grep 不兼容
  ──────────────────────────────────  ──────────  ──────────────────────────────────────────────────────
   WebFetch                              约 70%    已有 SSRF、提取、SearXNG，但契约和名称不兼容
  ──────────────────────────────────  ──────────  ──────────────────────────────────────────────────────
   Bash/命令执行                         约 45%    有 Bubblewrap 与 Skill script runner，但没有生产级通
                                                   用 Bash
  ──────────────────────────────────  ──────────  ──────────────────────────────────────────────────────
   Python/CMake/Make/Curl                约 20%    可执行程序存在，但没有类型化、受控 Tool 实现
  ──────────────────────────────────  ──────────  ──────────────────────────────────────────────────────
   Skill 清单兼容                        约 55%    v0 allowed-tools 原样复制；v1 与 portable 字段没有统
                                                   一规则
  ──────────────────────────────────  ──────────  ──────────────────────────────────────────────────────
   Skill 理解与资源读取                  约 60%    可注入单个 Skill 正文、读取声明资源，但没有通用
                                                   Skill 工具，不能按常见工作流渐进加载
  ──────────────────────────────────  ──────────  ──────────────────────────────────────────────────────
   Skill 多根发现                        约 50%    环境变量或 Cursor 两个目录；
                                                   缺 .claude/.codex/.agents 等统一发现和优先级
  ──────────────────────────────────  ──────────  ──────────────────────────────────────────────────────
   长时间工具生命周期                    约 75%    已有 durable invocation 内核，但尚未接入 Bash 等通用
                                                   工具
  ──────────────────────────────────  ──────────  ──────────────────────────────────────────────────────
   AgentTemplate 生产接入                约 50%    仍存在 callback runner、宽权限、危险 attach→run 等缺
                                                   口

  当前未跟踪内容包括 af-target-v2.md 和 docs/tools/。我会保留它们，不覆盖或删除。

  ## 关键技术口径

  不能把 Read 简单注册为 fs_read 的第二个名字。正确模型应为：

  Skill 中的外部名称
  Read / Edit / Bash / Python
            │
            ▼
  Canonical Tool Capability
  fs.read / fs.edit / process.shell / process.python
            │
            ▼
  版本化适配器和生产实现
  Filesystem / Sandbox / Durable Invocation / Web Runtime

  权限、allowlist、Hook、审计和运行收据都按 canonical capability 判断，同时保留原始请求名称。这样才能避
  免通过 Read 与 fs_read 的名称差异绕过权限。

  “兼容 Claude Code、Codex、Cursor”也需要定义为 AF 的版本化兼容 Profile，而不是声称三者存在完全相同的正
  式标准。首期以广泛使用的 Skill 写法和 Claude/Cursor 工具名为公共契约，同时为 Codex 风格工具语义提供适
  配。

  ## AT0→AT9 修订实施计划

  ### AT0：冻结兼容基线

  - 建立 agent.portable_tool_profile/v1。
  - 定义 canonical ID、外部名称、历史名称、Schema revision。
  - 首批覆盖：
      - Read、Write、Edit、Glob、Grep
      - Bash、Python、CMake、Make、Curl
      - WebFetch、WebSearch
      - Skill

  - 在目标文档中建立逐项验收矩阵。

  ### AT1：Tool Identity 与安全 alias

  - ToolBus 引入 canonical capability 和 alias registry。
  - 注册时检查冲突、循环 alias 和 Schema 不一致。
  - 调用时先解析 canonical identity，再进行：
      - 环境 allowlist
      - Skill permission
      - Hook
      - 参数重写后重新授权
      - 审计和 receipt

  - receipt 同时记录 requested name、canonical ID、revision。
  - MCP 名称兼容 mcp__server__tool，保留旧格式迁移能力。

  ### AT2：Skill 清单和多根发现

  - v0/v1 都识别常见顶层 allowed-tools。
  - 将 allowed-tools 和原生 permissions.tools 编译为 canonical 请求权限。
  - 未知名称、冲突字段、废弃名称产生明确诊断。
  - 支持确定性 Skill 根目录发现：
      - 项目 .agents/skills
      - .claude/skills
      - .cursor/skills
      - .codex/skills
      - 用户显式配置目录

  - 定义覆盖优先级、重名冲突和 revision pinning。

  ### AT3：文件工具兼容层

  实现真正的兼容适配器，而不只是改名：

  - Read：offset/limit、行号、输出上限、文本及受控二进制预览。
  - Write：原子写入、覆盖策略、expected revision。
  - Edit：精确匹配、唯一性检查、replace-all、digest/mtime 并发保护。
  - Glob：路径、pattern、ignore、排序、数量限制。
  - Grep：regex、glob、上下文、结果边界。
  - workspace 可写；已激活 Skill package 只读。
  - Mkdir/Touch/Remove：类型化目录创建、文件时间戳/创建和显式确认删除；Remove
    默认非递归，禁止删除 workspace 根、拒绝符号链接并支持 expected revision。
  - 补齐 traversal、symlink、TOCTOU、并发编辑测试。

  ### AT4：统一 Process Tool Runtime

  建立一个 ProcessToolRunner，其上实现：

  - Bash：命令、cwd、timeout、background、取消和输出分片。
  - Curl、Wget：类型化网络请求/下载，共用域名权限、SSRF、重定向与输出边界。
  - Sed：类型化文本转换，默认预览；写入受 workspace jail、审批与 revision 保护。
  - LS、Cat、Grep：兼容常见 Skill 命令名，分别复用 Glob、Read、Grep 的 canonical 权限域。
  - Python：固定 Python3 解释器和参数数组，不拼接 shell 字符串。
  - CMake、Make：固定 executable、受控工作目录和参数。
  - Curl：URL/headers/method/output 等类型化输入。
  - 默认使用 Bubblewrap、最小环境变量和 cwd jail。
  - 网络默认关闭，按工具和策略显式授权。
  - background 接入 durable invocation 的 attach/cancel/reconcile/restart。
  - 输出超限时摘要并外置到 ObjectStore。
  - attach() 不得默认退化为重新执行。

  当前实现证据（2026-08-14）：

  - canonical `Bash/Python/CMake/Make` 已接入 Bubblewrap deny-network workspace sandbox；
    `Curl/Wget` 已复用 WebFetch 的 SSRF、域名、重定向和响应上限策略。
  - `Sed/LS/Cat/Grep` 复用类型化文件工具；`Mkdir/Touch/Remove` 已纳入同一权限域，
    Remove 具备确认门、默认非递归、根目录保护、symlink 拒绝及 revision CAS。
  - 已通过 `process_tools_builtin`、`fs_tools_builtin`、`skill_policy_runtime_contract`
    和 `toolbus_wp2` 专项回归。
  - 尚待闭合：后台进程的 durable attach/cancel/reconcile、Curl 非 GET method、Wget
    原子落盘，以及大输出 ObjectStore 外置；不得据此将 AT4 标为完成。

  NET0–NET8 修订进度（2026-08-14）：已建立通用 `WebHttpRequest`（受控方法、请求体、
  响应头、重定向计数）及 opaque `credential_ref` broker 接口；Curl 已支持
  GET/HEAD/POST/PUT/PATCH/DELETE/OPTIONS、JSON/text body、超时和响应上限，Wget 已支持
  workspace 原子落盘与覆盖门。用户参数禁止 Authorization/Cookie/Proxy-Authorization/Host
  及 CRLF header；跨 origin credential redirect fail-closed。仍未闭合 Basic/API-key/mTLS、
  retry/backoff、Range/ETag resume、标准 SHA-256/SHA-512 校验、流式大文件/ObjectStore、
  durable attach/cancel/reconcile，故本项继续保持 partial。

  NET4–NET6 增量（2026-08-14）：Curl 增加 Basic/API-key、expected-status、HTTP error gate、
  安全 retry/backoff 与非幂等 idempotency-key 门；Wget 增加 Range/If-Range、ETag sidecar、
  206 append、expected-size、标准 SHA-256/SHA-512 和校验后原子发布。Curl/Wget 均可把
  响应外置到注入的 content-addressed ObjectStore。现有同步 ToolBus handler 仍不是 durable
  download worker；真正流式大文件、重启 attach/cancel/reconcile 继续列为 NET7 blocker。

  NET7–NET8 增量（2026-08-14）：新增 production `NetworkToolExecutionAdapter`；Wget 固定
  `RestartFromCheckpoint` 且要求 `resume=true`/幂等 invocation，Curl 固定 `ManualReview`
  并将非幂等失败归类为 `remote_effect_unknown`。HTTP GET 接收现支持流式 sink 与逐 chunk
  cooperative cancellation，Wget 使用 `.transfer → .part → verified final` 管线，不再将完整
  下载响应驻留内存。targeted `phase4_execution_adapters/process_tools_builtin` 等 5/5 PASS。
  尚需在允许 loopback socket 的执行环境完成真实 Bearer/Basic/API-key、跨域 redirect、Range、
  断连恢复和慢流 cancel 网络矩阵；本受限 sandbox 无法绑定测试监听端口，因此该 live transport
  certification 不得以当前离线测试替代。

  ### AT5：Web 与 Skill 理解工具

  - WebFetch 适配常见 url + prompt 契约。
  - 保留现有 SSRF、重定向校验、域名权限和 HTML→Markdown。
  - WebSearch 继续默认 SearXNG，并抓取结果页面正文。
  - 新增 Skill 工具：
      - 列出/搜索 Skill；
      - 显式加载完整 SKILL.md；
      - 返回版本、来源、权限和资源目录；
      - 在当前 Skill jail 内读取 references/scripts/assets；
      - 支持渐进披露而非一次注入全部内容。

  - 主 Skill 文件不再因“不是声明资源”而无法受控读取。

  ### AT6：AgentTemplate 生产接线

  - ToolBus runner 连接 canonical tools。
  - SandboxedProcess runner 连接 AT4。
  - 实现节点级有效权限：

  Template grant
  ∩ invocation grant
  ∩ pinned Skill grant
  ∩ node request
  ∩ runtime policy

  - 生产 composition 默认启用这些 runner。
  - 禁止 callback/test runner 通过 production certification。
  - 修复 compiler 的条件、重试、失败策略、幂等和 approval 语义。

  AT5–AT6 增量（2026-08-14）：新增 canonical `Skill` 渐进披露工具（list/search/load/
  declared-resource read），主 SKILL.md 可在 pinned package jail 内分页读取，其他资源仍强制
  manifest/digest。AgentTemplate 新增 production ToolBus runner、runner origin gate，production
  registry 拒绝 callback；节点有效权限按 session∩pinned-skill∩node-request 收窄。Compiler 已落实
  deterministic condition、bounded retry、effectful retry idempotency gate、optional continue、output
  schema 及 artifact/evidence required gate。尚待接入 MCP/ChildAgent/Approval/NestedWorkflow 等
  production runner 和默认 ProductionAgentRuntimeBuilder，AT6 保持 partial。

  AT6–AT9 增量（2026-08-14）：`build_production_toolbus_runners` 已默认提供
  LocalCapability/SandboxedProcess/CLI/MCP 四类真实 ToolBus runner，并统一 alias→canonical 授权、
  cancellation、异常结构化与四阶段 receipt/event；ChildAgent/NestedWorkflow/HumanApproval 在没有真实
  后端时仍 fail-closed，不允许 callback 冒充生产实现。五个正式 demo 的 `LiveRuntime` 已统一持有该
  production registry。`LiveOperationsProjection` 现可直接消费 canonical durable
  `InvocationEventSubscription`，支持 replay cursor、幂等去重、progress/terminal/manual-review 投影和
  SQLite snapshot 持久化，修复长工具运行时 observation snapshot 不更新的事件源断裂。五 demo 全部
  编译通过，portable-tools/Skill/AgentTemplate/LTW/UI 定向回归 17/17 通过。尚未闭合项是：真实
  ChildAgent/NestedWorkflow/HumanApproval backend composition、LLM candidate/token/cost 的完整事件字段，
  以及涉及 UI 视觉改变时的真实截图认证；因此总体 AT0–AT9 仍应标记为 partial，而非生产全认证。

  AT6/AT7 继续收口（2026-08-14）：新增依赖注入式 `ChildTaskSkillRunner`、
  `NestedWorkflowSkillRunner` 与 `ApprovalSkillRunner`。三者只在部署分别提供真实
  `ChildTaskBackend`、`NestedWorkflowPort`、durable `ApprovalStore` 时注册；未决审批返回
  `Waiting/awaiting_approval`，不会被自动批准或误报为普通工具成功。Tool runner 事件现记录
  requested→canonical 名称、权限决定、input/output digest 与 latency。LLM telemetry 在部署 allowlist
  允许时额外导出 selected candidate、逐 attempt fallback/失败摘要、usage source/unknown reason 及输入输出
  digest，同时保持旧 telemetry policy 兼容和 fail-closed privacy gate。

  Durable interruption 增量（2026-08-14）：AgentTemplate compiler 不再把 HumanApproval 的未决状态
  当作普通失败并取消整图，而是返回显式 `suspended`、`checkpoint_ref` 和 resume snapshot。快照保存已
  完成节点输出，恢复时跳过这些节点，避免重复执行前驱副作用；快照强制绑定 plan digest、invocation、
  session 并校验 canonical digest，篡改后 fail-closed。审批消费同时校验 request/decision expiry、
  revoked/expired 终态及 request/plan/arguments/policy 四重绑定。

  Restart recovery 增量（2026-08-14）：SQLite AgentTemplate registry schema v2 新增 execution
  checkpoint 表与 create/update/delete CAS、canonical digest 读校验。暂停时 durable 保存 plan、pinned
  session、root input、已完成 node outputs 和等待节点；`AgentRuntime::resume(tenant, invocation)` 可在
  registry 重开后恢复执行。恢复成功仍须经过 completion authority，只有验收接受后才 CAS 删除
  checkpoint；并发恢复或过期 revision 均返回冲突，不会双重提交或提前丢失恢复依据。

  ### AT7：长任务与可观测性

  - 把 Bash/Python/CMake/Make/Curl 生命周期投影到统一 Event Subscription/Replay。
  - 事件覆盖 queued、started、progress、checkpoint、completed、failed、cancelled、unknown-effect。
  - LLM observability 记录：
      - 工具候选与选择；
      - alias→canonical 解析；
      - 权限决策；
      - 输入/输出 digest；
      - token、延迟、重试和失败原因。

  - UI observation snapshot 使用同一事件源实时更新。

  ### AT8：系统集成和迁移

  执行与认证证据统一记录在 `docs/at8-at9-certification.md`；状态必须区分 implemented、exercised、
  certified 和 external-not-exercised，不允许以编译通过替代真实运行认证。

  - 五个正式 demo 统一使用 production runtime builder。
  - 旧 fs_*、web_* 保留一个迁移周期，但导出时优先公共名称。
  - 提供迁移诊断，不静默扩大权限。
  - 用未经修改的 Claude/Cursor/Codex 风格 Skill fixture 验证可移植性。
  - 覆盖“Skill→Read/Edit→Bash/CMake/Make→验证→WebFetch”的完整任务。

  ### AT9：系统性认证

  测试分为：

  - 契约测试：名称、Schema、manifest、revision。
  - 功能测试：每个工具的正常和边界路径。
  - 安全测试：alias 绕权、目录逃逸、symlink、SSRF、命令注入、环境泄漏。
  - 生命周期测试：超时、取消、后台运行、进程崩溃、重启恢复、重复 attach。
  - 并发测试：Edit 冲突、权限变更、revision drift。
  - Skill 兼容测试：同一 SKILL.md 不修改即可运行。
  - AgentTemplate 集成测试：planner→Skill→tools→assurance→closure。
  - 回归测试：现有 Skill、ToolBus、Web、Harness、LTW、Phase 4 测试。
  - UI 如发生变化，启动真实 Web/TUI/ImGui 环境并提交真实截图验收。

  建议把本轮重新定义为“Portable Tools & Skills Closure”，不要继续沿用旧 AT0→AT9 已完成的表述，以免与原
  AgentTemplate 阶段混淆。可命名为 AT-V2-PT0 → PT9，但若你希望保持原编号，也可以直接修订现有计划。

  请明确批准后，我将先落盘修订计划，再按 AT0→AT9 连续
