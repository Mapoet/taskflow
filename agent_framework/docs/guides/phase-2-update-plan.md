 SQLite 在当前环境可用（3.31.1），但仓库只声明过未落地的 SQLite memory backend，尚无通用数据库封装；AgentThreadState 也没有完整序列化协议。因此计划必须新增独立 session
  persistence 层，不能复用现有 memory stub。还需锁定数据库默认启用方式和敏感会话数据的保护边界，这会直接决定部署兼容性与 DoD。

• Questions 3/3 answered
  • AgentServer 的 SQLite SessionStore 默认如何启用？
    answer: Server默认持久化 (Recommended)
  • Phase 2 对 SQLite 中的会话正文采用什么保护策略？
    answer: 权限+可插拔加密 (Recommended)
  • SQLite schema 升级采用哪种策略？
    answer: 版本表顺序迁移 (Recommended)

• Proposed Plan


  # Phase 2 深度集成升级实施计划

  ## 1. 目标与约束

  将当前“组件完成、集成未完成”的 Phase 2 收敛为一条统一生产链：

  CLI / UI / AgentServer / A2A
              |
     GraphExecutor::execute
              |
   InputPolicy -> AgentLoop -> Tool/ChildTask
              |
         Memory -> Verifier
              |
   SQLite Session Commit
              |
    ExecutionEvent -> SSE/UI/Audit

  完成标准：

  - CLI、UI、AgentServer 共用相同执行入口和状态提交语义。
  - 移除 AgentServer::set_task_handler，不保留双轨生产模式。
  - Server 默认使用 SQLite 持久化 session；CLI/UI 默认内存。
  - Tier B 必须实现和接线，但默认关闭。
  - A2A streaming、session 延续、取消、Verifier、Memory、Subflow 形成端到端闭环。
  - Local Subflow 与 Remote A2A task 使用统一 child-task 生命周期。
  - 五组 Phase 2 垂直集成测试全部通过。

  ## 2. 公共接口与数据模型

  ### 2.1 统一执行接口

  新增：

  struct ExecutionRequest {
      std::string template_id;
      AgentConfig config;
      AgentWorkflowDeps deps;
      std::shared_ptr<internal::AgentThreadState> session;
      ExecutionContext context;
      std::shared_ptr<TaskControl> control;
      std::shared_ptr<SessionStore> session_store;
      ExecutionEventSink event_sink;
      ExecutionOptions options;
  };

  struct ExecutionResult {
      bool success;
      int exit_code;
      json outputs;
      std::optional<std::string> error;
      std::string session_id;
      std::uint64_t committed_revision;
      std::optional<std::string> checkpoint_id;
      ExecutionTerminalStatus status;
  };

  class GraphExecutor {
  public:
      ExecutionResult execute_sync(tf::Executor&, ExecutionRequest);
      std::future<ExecutionResult> execute_async(tf::Executor&, ExecutionRequest);
  };

  规则：

  - run_react_cli_sync/async 保留一轮兼容周期，但内部只能调用 execute_sync/async。
  - 删除旧 execute(const std::string&) 和 build_custom_workflow 占位接口。
  - WorkflowTemplate 改为接收 ExecutionRequest、GraphBuilder 和 terminal sink；模板只负责构图，GraphExecutor 统一负责输入处理、Verifier、commit 和事件。
  - 模板不存在、依赖缺失、session revision 冲突均返回结构化错误，不抛出到 HTTP 线程。

  ### 2.2 AgentServer 运行配置

  删除：

  AgentServer::set_task_handler(...)

  新增：

  struct AgentExecutionProfile {
      std::string template_id = "react_cli";
      AgentConfig config;
      AgentWorkflowDeps deps;
      InputPolicyConfig input_policy;
      VerifierConfig verifier;
  };

  void AgentServer::set_execution_profile(AgentExecutionProfile);
  void AgentServer::set_graph_executor(std::shared_ptr<GraphExecutor>);
  void AgentServer::set_session_store(std::shared_ptr<SessionStore>);

  行为：

  - start() 前缺少执行 profile、GraphExecutor 或必要依赖时立即失败。
  - Server 继续拥有 Taskflow executor 和有界 dispatch queue。
  - 每个 A2A task 创建独立 ExecutionRequest，不跨请求共享 GraphBuilder。
  - 所有现有 demo 和测试一次性迁移，不保留旧 handler 兼容实现。

  ### 2.3 SQLite SessionStore

  接口：

  struct SessionSnapshot {
      std::string session_id;
      std::uint64_t revision;
      std::string checkpoint_id;
      internal::AgentThreadState state;
      std::vector<ToolCommitRecord> tool_commits;
      std::vector<ChildTaskSnapshot> child_tasks;
  };

  class SessionStore {
  public:
      virtual SessionSnapshot load_or_create(std::string_view session_id) = 0;
      virtual CommitResult commit(
          const SessionSnapshot& next,
          std::uint64_t expected_revision) = 0;
      virtual std::optional<SessionSnapshot> load_checkpoint(
          std::string_view session_id,
          std::string_view checkpoint_id) = 0;
  };

  SQLite v1 schema：

  schema_version(
    version INTEGER PRIMARY KEY,
    applied_at TEXT NOT NULL
  )

  sessions(
    session_id TEXT PRIMARY KEY,
    revision INTEGER NOT NULL,
    checkpoint_id TEXT NOT NULL,
    state_payload BLOB NOT NULL,
    terminal_status TEXT NOT NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
  )

  tool_commits(
    session_id TEXT NOT NULL,
    tool_call_id TEXT NOT NULL,
    attempt INTEGER NOT NULL,
    status TEXT NOT NULL,
    result_digest TEXT,
    committed_at TEXT NOT NULL,
    PRIMARY KEY(session_id, tool_call_id, attempt)
  )

  child_tasks(
    session_id TEXT NOT NULL,
    child_id TEXT NOT NULL,
    backend TEXT NOT NULL,
    attempt INTEGER NOT NULL,
    status TEXT NOT NULL,
    payload BLOB NOT NULL,
    updated_at TEXT NOT NULL,
    PRIMARY KEY(session_id, child_id, attempt)
  )

  持久化规则：

  - Server 默认数据库路径由 AGENT_SESSION_DB 指定；未设置时使用运行目录下 .agent/session.db。
  - CLI/UI 默认 InMemorySessionStore，显式设置路径后可使用 SQLite。
  - 数据库目录权限 0700，文件权限 0600。
  - 提供 SessionPayloadCodec 加密/解密回调；默认 identity codec，不在框架内管理密钥。
  - AgentThreadState 使用版本化 JSON 序列化；不直接序列化 live pointer。
  - outbound_supervisor 只保存可恢复 child-task snapshot，加载时由 runtime factory 重建。
  - 使用 BEGIN IMMEDIATE 和 expected revision 实现 compare-and-commit。
  - cancel、deadline、build error、body error、Verifier abort 不提交部分 session。
  - SQLite busy 使用有限重试；超限返回 session_store_busy。
  - schema 使用顺序迁移；数据库版本高于当前程序时拒绝启动。
  - v1 migration 在事务内创建全部表和索引，不自动删除未知数据。

  ### 2.4 统一事件模型

  新增：

  enum class ExecutionEventType {
      TaskSubmitted,
      TaskStarted,
      TaskStatusChanged,
      ToolStarted,
      ToolCompleted,
      ChildTaskUpdated,
      VerifierStarted,
      VerifierCompleted,
      MemoryCompacted,
      ArtifactUpdated,
      CheckpointCommitted,
      ExecutionCompleted
  };

  struct ExecutionEvent {
      ExecutionEventType type;
      std::string task_id;
      std::string session_id;
      std::string run_id;
      std::optional<std::string> child_id;
      std::uint64_t sequence;
      std::string timestamp;
      json payload;
  };

  using ExecutionEventSink = std::function<void(const ExecutionEvent&)>;

  约束：

  - 每个 run 内 sequence 单调递增。
  - 事件 sink 异常不得破坏执行，记录后继续。
  - SSE、CLI/UI、audit log 和测试 collector 都是 adapter。
  - Verifier、Memory、ToolBus、OutboundTaskSupervisor 不再自行定义平行事件格式。
  - 线路 payload 统一应用 WP2.1c wire cap。

  ### 2.5 Tier B 输入策略

  新增 InputPolicyConfig：

  struct InputPolicyConfig {
      bool tier_b_enabled = false;
      std::shared_ptr<LLMClient> tier_b_llm;
      int tier_b_timeout_ms = 3000;
      int tier_b_max_calls_per_request = 1;
      TierBFailureMode failure_mode = TierBFailureMode::FallbackTierA;
  };

  规则：

  - Tier B 为 Phase 2 必达功能，但默认关闭。
  - 启用时缺少独立 LLM 配置立即拒绝启动，不回退使用主模型。
  - 输出只允许 allow/rewrite/reject 三种结果。
  - timeout、解析失败按配置降级，默认保留 Tier A 结果。
  - CLI 与 A2A 使用同一 InputPolicy pipeline。
  - reject 映射为 JSON-RPC invalid_params，并发出 InputPolicy 事件。

  ### 2.6 统一 ChildTask API

  新增：

  ChildTaskRequest
  ChildTaskHandle
  ChildTaskStatus
  ChildTaskResult
  ChildTaskUsage
  ChildTaskPolicy
  ChildTaskBackend

  统一字段：

  - parent_run_id
  - child_id
  - attempt
  - iteration
  - depth
  - deadline
  - iteration_budget
  - tool_call_budget
  - trace_context
  - idempotency_key

  Backend：

  - LocalSubflowBackend：包装 SubflowNode/SubflowModule。
  - A2aRemoteBackend：包装 OutboundTaskSupervisor。
  - submit/wait/cancel/retry/restart/resume 对上层保持一致。
  - 父取消和 deadline 递归传播。
  - 只有成功 attempt 可以替换父图可见结果。
  - Remote backend 不支持的 resume 必须返回 unsupported_resume，不得隐式重发副作用请求。

  ## 3. 分阶段实施

  ### Stage 1：统一执行主干与 SQLite session

  1. 为 AgentThreadState 增加版本化序列化，明确 transient/live 字段。
  2. 实现 SessionStore、InMemorySessionStore、SQLiteSessionStore 和 schema migration。
  3. 引入 ExecutionRequest/Result，把现有 ReAct 构图与 Verifier 逻辑迁入 execute_sync。
  4. 将 run_react_cli_sync/async 改为薄包装并保持原测试兼容。
  5. 重构 WorkflowTemplate 注册与执行；删除只发现不执行的模板行为。
  6. AgentServer 增加 execution profile、GraphExecutor 和 SessionStore。
  7. 删除 set_task_handler，迁移所有 Server 测试、demo 和 loopback fixture。
  8. A2A contextId 作为 session 主键；缺失时 Server 生成并在响应中返回。
  9. 每次执行从 store 加载 revision，成功后 CAS commit；冲突返回可重试错误。
  10. 增加 Server 重启后第二轮 history 延续测试。

  退出标准：

  - CLI 与 A2A 对同一 mock 输入产生相同 final/history/iteration。
  - Server 重启后可从 SQLite 延续第二轮。
  - cancel/失败不增加 revision。
  - 原 set_task_handler 全仓无引用。

  ### Stage 2：A2A v1 Card 与 streaming 闭环

  1. 先修正 Card：优先读写 supportedInterfaces[]，旧顶层 url 仅解析兼容，不再作为生产输出。
  2. 补充 Client 常量：SendStreamingMessage、SubscribeToTask 和标准 subscribe path。
  3. streaming 方法不进入普通一次性 DispatchTable 返回路径；HTTP transport 识别流式 method 并建立 chunked SSE response。
  4. SendStreamingMessage 创建 task 后立即订阅同一 channel。
  5. SubscribeToTask 对已存在 task 重放当前 snapshot，随后发送增量事件。
  6. 每个 SSE data 只包含一个规范 StreamResponse。
  7. 支持断线清理、terminal 自动关闭、Last-Event-ID 解析；v1 不保证历史事件完整重放，只重发当前 snapshot。
  8. AgentClient 新增 callback/consumer 风格 streaming API。
  9. 保留 legacy endpoint 一个迁移周期，但默认关闭；文档记录删除版本。
  10. 增加 Card capability、dispatch method、Client 常量一致性测试。

  退出标准：

  - 规范 Client 无需 legacy REST 即可完成 send/get/cancel/stream/subscribe。
  - Card、tracker、Server、Client 不存在 method/path 漂移。
  - 多事件 fixture 与回环测试通过。

  ### Stage 3：事件总线、Tier B、Verifier、Memory 和取消

  1. 实现 thread-safe sequenced ExecutionEventEmitter。
  2. AgentServer event adapter 自动发布 Task/Artifact/Verifier/Memory/Tool/ChildTask SSE。
  3. 替换 on_verifier_event 和零散 memory/supervisor callback；兼容回调只保留在内部 adapter。
  4. 将 Tier A/B 移到 GraphExecutor 统一入口，Server 不再提前运行另一套预处理。
  5. Tier B 使用独立 LLM、独立 timeout、最大一次调用和固定 schema。
  6. TaskControl/stop_token 贯穿 LLM invoke、ToolBus call、Local Subflow、A2A Client 和 Verifier。
  7. 所有网络调用使用剩余 deadline，而非各自重新计算 timeout。
  8. 无法中断的第三方调用放入隔离 worker；取消后丢弃结果并禁止 commit。
  9. Verifier retry_main 使用同一 run/session identity，不重复 user turn。
  10. Memory compact 发出 before/after/strategy 事件，事件 payload 应用预算。
  11. 添加 event sink 背压策略：单 session 有界队列，非终态高频事件可丢弃并记录计数，terminal/commit 事件不可丢。

  退出标准：

  - A2A 可观察 InputPolicy、Tool、Memory、Verifier 和 terminal 的有序事件。
  - cancel 能在 deadline 内结束框架可控调用。
  - cancel 后 session revision、history 和副作用 commit 不变。

  ### Stage 4：统一本地与远程子任务

  1. 定义 ChildTask 公共类型、状态机和 backend 接口。
  2. 将 SubflowNode 适配为 LocalSubflowBackend。
  3. 将 OutboundTaskSupervisor 适配为 A2aRemoteBackend。
  4. AgentLoop 工具面改为统一 child submit/status/wait/cancel API。
  5. 保留现有 a2a.* 工具名作为 Remote backend adapter，内部不再复制生命周期逻辑。
  6. 统一 parent context、预算、depth、attempt、trace 和 idempotency key。
  7. child snapshot 写入 SQLite child_tasks。
  8. 新用户轮次按固定策略 cancel 所有未终态且未声明保留的 child task。
  9. retry 重跑同一逻辑 attempt；restart 创建新 attempt；resume 只从已提交 checkpoint 继续。
  10. 部分失败以逐 child 结果聚合，不将整个 fan-out 简化为 boolean。

  退出标准：

  - 同一父 Agent 可在不改变控制代码的情况下选择 Local 或 A2A backend。
  - 两个 child 并发、一个失败、一个取消时结果和 usage 可确定聚合。
  - Server 重启后能恢复 child terminal snapshot，不重复提交已完成远程任务。

  ### Stage 5：恢复、幂等和安全加固

  1. checkpoint commit 与 session commit 使用同一 SQLite 事务。
  2. tool_call_id + attempt 建立唯一提交记录。
  3. 副作用工具执行前检查 committed/in-flight 状态。
  4. 进程崩溃后，未知状态的副作用调用标记 reconciliation_required，不得自动重放。
  5. ReadOnly 工具可按 policy 自动重试；Write/Unknown 默认禁止自动重试。
  6. SQLite payload codec 接入可插拔加密回调。
  7. 数据库打开时校验权限；权限过宽时 Server 默认拒绝启动，可通过明确开发开关降级为警告。
  8. 增加数据库损坏、migration 回滚、busy、revision conflict 和未知高版本测试。
  9. session 删除、过期和 vacuum 只提供显式管理 API，不在请求线程自动执行。

  退出标准：

  - 进程重启后的 ResumeFromCheckpoint 不重复 history、tool result 或副作用。
  - migration 失败不留下半升级数据库。
  - 未配置加密时仍可运行，但权限门禁生效。

  ### Stage 6：文档、CI 与旧接口清理

  1. 更新 phase-2-plan.md 和全部 phase-2-wp*.md。
  2. DoD 改用 [ ]/[~]/[x]/[!] 四态并链接代码、CTest 和验证日期。
  3. 删除重复 M4。
  4. 更新 A2A tracker 的 Card、streaming 和 legacy 状态。
  5. 更新 Server、Client、SessionStore、Tier B、Verifier、Memory 和多 Agent 用户指南。
  6. 增加聚合 CTest labels 与 CI job。
  7. 全仓扫描禁止 set_task_handler、未实现 streaming method 和旧 GraphExecutor 占位。
  8. 记录 SQLite schema version、A2A fixture revision、测试总量和平台限制。

  ## 4. 测试与验收

  ### 单元与组件测试

  - Session serialization round-trip，覆盖 history、ExecutionContext、pending 输入、Verifier 和 memory 字段。
  - SQLite create/load/CAS conflict/migration/rollback/busy/corruption/权限。
  - Execution template 查找、依赖校验、同步/异步异常传播。
  - Tier B allow/rewrite/reject/timeout/invalid JSON。
  - Event sequence、sink exception、queue overflow、terminal 不丢失。
  - ChildTask 状态机、预算、depth、attempt 和 backend 能力差异。
  - A2A Card supportedInterfaces round-trip 和旧 Card 只读兼容。

  ### 垂直集成标签

  - phase2-cli-e2e：CLI -> execute -> AgentLoop -> Verifier -> SQLite commit。
  - phase2-a2a-e2e：Client -> Server -> execute -> SSE -> Server restart -> second turn。
  - phase2-quality-e2e：Tier A/B -> budget -> tools -> memory -> Verifier retry。
  - phase2-multi-agent-e2e：Local + Remote child 并发、部分失败、cancel 和 usage 聚合。
  - phase2-recovery-e2e：checkpoint、崩溃模拟、tool-call 去重、revision conflict。

  ### 最高价值组合场景

  A2A SendStreamingMessage
  -> @file 输入质控
  -> Tier B rewrite
  -> AgentLoop 三轮工具调用
  -> Local Subflow + Remote A2A child
  -> memory auto compact
  -> Verifier retry_main
  -> ordered SSE events
  -> SQLite commit
  -> Server restart
  -> second-turn resume
  -> cancel one child
  -> final/history/usage/tool effects 无重复

  ### 全量门禁

  - Debug 全量构建。
  - 全部现有离线 CTest。
  - 五组 Phase 2 labels。
  - Workflow/Agent TSAN 重点集。
  - SQLite migration 和重启恢复测试。
  - git diff --check。
  - 静态扫描无旧 handler、GraphExecutor 占位和 streaming method_not_found。
  - A2A loopback 网络测试标记资源锁或 RUN_SERIAL，禁止依赖偶然端口时序。

  ## 5. 实施顺序与提交边界

  按以下独立可回滚切片提交：

  1. Execution API 与 AgentThreadState serializer。
  2. SQLite SessionStore、migration 和测试。
  3. GraphExecutor 模板执行与 CLI 包装迁移。
  4. AgentServer 切换 GraphExecutor并删除 task handler。
  5. A2A Card v1 与 streaming Server。
  6. Streaming Client 与 contract fixture。
  7. ExecutionEvent 总线及 SSE/UI/Audit adapter。
  8. Tier B、Verifier、Memory 统一接线。
  9. stop token/deadline 全链传播。
  10. ChildTask 公共协议与 Local backend。
  11. Remote A2A backend 与持久化 child snapshot。
  12. checkpoint/tool idempotency、安全加固。
  13. 垂直测试、文档状态和 CI 门禁。

  每个切片必须保持编译和既有测试通过；第 4 个切片允许一次有意的公共 API breaking change，但必须在同一提交迁移全部仓内调用方。

  - AgentServer::set_task_handler 立即移除，不长期保留兼容双轨。
  - Server 默认 SQLite 持久化；CLI/UI 默认内存。
  - SQLite 是 Agent Framework Server 构建的必需依赖，使用 CMake find_package(SQLite3 REQUIRED)。
  - SQLite schema 使用版本表和事务内顺序迁移。
  - 数据库默认文件权限 0600，并提供可插拔 payload 加密 codec；框架不管理密钥。
  - Tier B 为 Phase 2 功能和测试硬门禁，但默认关闭。
  - OAuth、RAG、长期记忆 Assembly、动态 MCP 仍不进入本升级范围。
  - legacy A2A 路径只保留一个明确迁移周期，默认关闭。
  - Phase 2 在五组垂直测试通过前保持 component-complete / integration-incomplete 状态。
