> 历史修复基线：本文的“当前问题”记录实施前状态；当前完成状态、代码证据与验证命令以 [phase-3-status.md](phase-3-status.md) 为准。

我按“罗列详细修复计划”理解。结论是：WP3.0–WP3.5 当前均存在不同程度的“组件已实现，但原始 DoD 未闭环”。建议按 WP3.0 → WP3.1 →
  WP3.2/3.3 → WP3.4 → WP3.5 修复，避免在不稳定的持久化和上下文模型上继续堆功能。

  ## 一、总体问题

  当前共同问题主要有五类：

  1. 文档状态与代码事实不同步，缺少可审计的完成矩阵。
  2. 单元组件测试较多，但跨组件端到端测试不足。
  3. 持久化实现偏“能保存”，尚未达到崩溃一致性和恢复安全。
  4. 部分安全接口已经声明，但默认策略、签名绑定范围和重启恢复仍不完整。
  5. 目前的 Phase 3 标签测试通过，只证明已有覆盖范围通过，不能证明原始 DoD 全部完成。

  ———

  # WP3.0：事实基线与文档校准

  ## 当前问题

  agent_framework/docs/guides/phase-3-update.md:3 仍标记为“待实施”。

  具体缺口：

  - Phase 2 文档没有全部改成 [ ]/[~]/[x]/[!] 四态。
  - 已实现的 SessionStore、Verifier、Tier B、ExecutionEvent 等仍可能被旧文档描述为未实现。
  - 缺少每项功能对应的源文件、CTest 名称和验证日期。
  - 缺少统一的 Phase 2、Phase 3 CTest 标签入口。
  - CI 没有明确区分：
      - 默认离线测试；
      - 可选 Faiss；
      - Live LLM/MCP；
      - UI 截图测试。

  - Demo 清单与 CMake 实际目标尚未形成单一事实源。

  ## 修复计划

  ### 3.0-A：生成事实矩阵

  新增 docs/guides/phase-3-status.md，每个工作包记录：

  - 状态；
  - 已实现能力；
  - 未实现能力；
  - 源文件；
  - 测试名称；
  - 最后验证日期；
  - 已知阻塞；
  - 回滚或迁移说明。

  ### 3.0-B：校准历史文档

  逐一检查：

  - phase-2-wp*.md
  - envs_status.md
  - agent_server_demo-update.md
  - phase-2-update.md
  - phase-3-update.md

  删除或标记为历史的内容：

  - mock-only AgentServer；
  - 已删除 Demo；
  - 已失效环境变量；
  - 已完成但仍标记未实现的功能。

  ### 3.0-C：统一测试标签

  至少形成：

  ctest --test-dir build -L phase2 --output-on-failure
  ctest --test-dir build -L phase3-offline --output-on-failure
  ctest --test-dir build -L phase3-faiss --output-on-failure
  ctest --test-dir build -L phase3-live --output-on-failure
  ctest --test-dir build -L phase3-ui --output-on-failure

  目前零散的 phase3-memory、phase3-vectorstore 等标签可以保留，但应同时增加聚合标签。

  ### WP3.0 验收

  - 文档中不存在与当前源码相冲突的“未实现”描述。
  - 所有公开 Demo 都能从 CMake 目标反查。
  - 每个 [x] 都有源文件和测试证据。
  - 默认 CI 不需要密钥、网络或 GUI。
  - Live/UI/Faiss 测试明确属于非默认门闩。

  ———

  # WP3.1：VectorStore 与 RAG

  ## 当前问题

  内存 VectorStore 和基本检索已经可用，但仍有以下缺口。

  ### Faiss 构建失败

  当前 Release 编译通过全局 add_compile_options 添加 -march=native，该选项会污染第三方 Faiss 子目录。即使设置
  FAISS_OPT_LEVEL=generic，Faiss 仍可能编译 AVX-512 路径并失败。

  ### Faiss 持久化契约不完整

  需要明确：

  - Faiss index 文件；
  - document/metadata sidecar；
  - ID 到 Faiss label 的稳定映射；
  - revision；
  - schema version；
  - digest；
  - 原子发布规则。

  仅保存 index 不能恢复文本、metadata 和 citation。

  ### 后端契约测试不足

  目前缺少：

  - Memory 与 Faiss 同数据集排序一致性；
  - upsert 后旧向量不可见；
  - delete 后无幽灵结果；
  - 损坏 index/sidecar 重建；
  - metadata sidecar 与 index revision 不一致；
  - 空库、NaN、零向量和非法维度。

  ### RAG 集成仍偏节点级

  KnowledgeBaseSourceNode 已经执行真实检索，但尚缺完整的：

  ingest → retrieve → assembly → prompt → citation → final output

  端到端路径。

  ## 修复计划

  ### 3.1-A：隔离编译参数

  把全局：

  add_compile_options(-O2 -march=native)

  改为只作用于项目目标：

  target_compile_options(agent_framework PRIVATE ...)

  Faiss 使用自己的 FAISS_OPT_LEVEL=generic，不得继承 -march=native。

  验证：

  cmake -S . -B /tmp/taskflow-faiss \
    -DAGENT_BUILD_FAISS=ON \
    -DCMAKE_BUILD_TYPE=Release
  cmake --build /tmp/taskflow-faiss --target test_vectorstore_faiss_contract

  ### 3.1-B：定义持久化格式

  建议目录：

  vector-index/
  ├── manifest.json
  ├── index.faiss
  ├── records.jsonl
  └── CURRENT

  manifest.json 至少包含：

  - schema_version
  - dimension
  - metric
  - index_type
  - revision
  - record_count
  - index_sha256
  - records_sha256
  - encoder_id
  - encoder_revision

  发布流程：

  1. 写入临时 generation 目录；
  2. fsync 文件；
  3. fsync 目录；
  4. 原子更新 CURRENT；
  5. 启动时只加载 CURRENT 指向的 generation。

  ### 3.1-C：统一后端契约测试

  同一套 fixture 对以下后端执行：

  - InMemoryVectorStoreBackend
  - FaissBackend

  比较：

  - 文档集合；
  - top-k 顺序；
  - score 允许误差；
  - metadata filter；
  - upsert/delete 结果。

  ### 3.1-D：补 RAG 端到端测试

  增加一个无网络 mock LLM 测试：

  1. 写入三条 GNSS 文档；
  2. 查询指定问题；
  3. 只召回相关 chunk；
  4. Assembly 装配 citation；
  5. Prompt 中不出现未召回全文；
  6. 最终结果带稳定 source/chunk ID。

  ### WP3.1 验收

  - 默认构建使用内存后端。
  - Faiss 可选构建成功。
  - 两后端通过同一契约测试。
  - 损坏 index/sidecar 可重建。
  - RAG 端到端 fixture 通过。

  ———

  # WP3.2：Memory Assembly

  ## 当前问题

  agent_framework/src/memory/memory_assembly.cpp:16 已实现确定性裁剪，agent_framework/src/node/agent_loop_node.cpp:439 也有初步
  接入，但还不是统一上下文装配层。

  主要问题：

  - 当前主要处理额外 context，未统一覆盖所有 slot。
  - working memory、retrieval、tool result、skill resource 仍可能各自直接进入 PromptRenderer。
  - Assembly 没有成为 PromptRenderer 前的唯一入口。
  - 缺少 memory_assembled 和 memory_evicted 事件。
  - 报告未贯通 GraphExecutor、audit 和调试接口。
  - /memory 与自动压缩未共享同一个预算模型。
  - token estimate 仍需与实际模型 profile 对齐。

  ## 修复计划

  ### 3.2-A：定义统一输入

  引入：

  struct MemoryAssemblyInput {
      std::vector<MemorySlot> system;
      std::vector<MemorySlot> task;
      std::vector<MemorySlot> working;
      std::vector<MemorySlot> retrieval;
      std::vector<MemorySlot> tool;
      std::vector<MemorySlot> skill;
  };

  ExecutionRequest 或 AgentLoop runtime 显式携带该输入。

  ### 3.2-B：建立唯一装配边界

  在 PromptRenderer 前执行：

  raw inputs
    → MemoryAssembly
    → assembled context
    → PromptRenderer
    → LLMClient

  禁止 RAG、Skills 和工具结果绕过 Assembly 直接追加 prompt。

  ### 3.2-C：完善预算策略

  分别配置：

  - hard total budget；
  - soft budget；
  - 各 slot quota；
  - 最低保留量；
  - priority；
  - eviction order；
  - citation retention；
  - tool result truncation；
  - system/task 不可驱逐规则。

  ### 3.2-D：增加事件

  扩展 ExecutionEventType：

  - MemoryAssembled
  - MemoryEvicted

  payload 只包含：

  - slot 数量；
  - token/byte 估算；
  - source ID digest；
  - 驱逐原因；
  - slot kind；
  - policy revision。

  不得记录原始文本。

  ### 3.2-E：补组合测试

  至少覆盖：

  - 六类 slot 同时存在；
  - 同输入重复执行结果一致；
  - hard budget 永不突破；
  - citation 不因正文裁剪而丢失；
  - system/task 优先保留；
  - tool result 比 system 更早驱逐；
  - 敏感文本不进入 report/event。

  ### WP3.2 验收

  - 所有 prompt context 必须经过 Assembly。
  - 六类 slot 均有真实生产调用方。
  - event/audit 可解释每次驱逐原因。
  - 同输入、同策略产生完全相同结果。

  ———

  # WP3.3：压缩策略与子 LLM

  ## 当前问题

  当前已有 truncate、extractive、structured 三个字符串分支，但仍属于条件选择器，并非真正策略注册表。

  主要缺口：

  - 没有 MemoryCompactor 抽象。
  - 没有 MemoryCompactorRegistry。
  - 不能由插件或测试注入新策略。
  - structured summary 未完全隔离主 LLM 配置。
  - 没有独立 token budget、deadline 和重试次数。
  - 无工具权限主要依赖调用约定，而非类型和 runtime 强制。
  - 原文 digest、摘要 schema、质量指标未形成稳定协议。
  - 取消、超时、非法 JSON 测试不足。

  ## 修复计划

  ### 3.3-A：抽象策略接口

  class MemoryCompactor {
  public:
      virtual ~MemoryCompactor() = default;
      virtual MemoryCompactionResult compact(
          const MemoryCompactionInput&,
          const MemoryCompactionContext&) = 0;
  };

  内置实现：

  - TruncateCompactor
  - ExtractiveCompactor
  - StructuredSummaryCompactor

  ### 3.3-B：实现注册表

  class MemoryCompactorRegistry {
  public:
      void register_compactor(std::string id,
                              std::shared_ptr<MemoryCompactor>);
      std::shared_ptr<MemoryCompactor> resolve(std::string_view id) const;
  };

  环境变量只负责选择 ID，不再直接控制内部分支。

  ### 3.3-C：隔离子 LLM

  定义独立 profile：

  - model/provider；
  - timeout；
  - max input/output token；
  - temperature；
  - retries；
  - 无 ToolBus；
  - 无 Skills；
  - 独立 TaskControl 子 deadline。

  structured 输出固定 schema：

  {
    "summary": "...",
    "facts": [],
    "open_items": [],
    "source_digest": "...",
    "schema_version": 1
  }

  ### 3.3-D：定义回退链

  structured
    ├─ success → validate schema → commit
    └─ fail/timeout/cancel/schema error
         → extractive
            └─ fail → truncate

  取消信号应直接停止，不应将用户取消误判为普通失败并继续调用子 LLM。

  ### 3.3-E：补测试

  - 注册和未知策略；
  - structured 成功；
  - 非法 JSON；
  - schema 字段缺失；
  - timeout；
  - cancellation；
  - LLM 抛异常；
  - extractive fallback；
  - truncate 最终兜底；
  - 重复压缩幂等；
  - source digest 一致；
  - token/secret 不进入事件。

  ### WP3.3 验收

  - 所有策略通过同一接口调用。
  - 子 LLM 无法访问工具。
  - 失败链有限且确定。
  - 取消不会触发额外 LLM 请求。
  - 所有输出都能追溯到原始内容 digest。

  ———

  # WP3.4：文件持久化记忆与恢复

  ## 当前问题

  当前 File/SQLite 后端具备基本读写，但距离生产恢复还有较大差距。

  agent_framework/src/memory/memory_store.cpp:22 当前直接追加 JSONL，没有 fsync 和 commit generation；agent_framework/src/
  memory/memory_store.cpp:35 仍为空。

  其他关键问题：

  - Message 接口没有显式 session ID，当前文件消息可能跨 session 混放。
  - 未实现 tenant/agent/session 隔离。
  - 没有 committed snapshot。
  - 没有 attempt/commit 边界。
  - 损坏 JSONL 只是停止读取，没有安全截断、隔离和 audit。
  - 没有 manifest 和内容 digest。
  - 没有 blob 管理和 GC。
  - 没有敏感字段统一脱敏。
  - 没有文件权限约束。
  - SQLite 的 schema migration/version 也不完整。

  ## 修复计划

  ### 3.4-A：修正存储 API

  把消息写入改成显式 session：

  store_message(const std::string& session_id,
                const Message& message);

  为兼容旧调用，可暂时保留旧重载，但旧重载只能用于明确的 default session，并输出迁移告警。

  ### 3.4-B：建立目录布局

  AGENT_MEMORY_DATA_DIR/
  └── tenant/
      └── agent/
          └── session/
              ├── CURRENT
              ├── events/
              ├── snapshots/
              ├── blobs/
              ├── indexes/
              └── manifest.json

  所有路径段必须经过安全 ID 校验，禁止 ..、绝对路径和符号链接逃逸。

  ### 3.4-C：实现 generation 提交协议

  写入流程：

  1. 写临时 generation；
  2. 写 events/snapshot/index manifest；
  3. 计算 SHA-256；
  4. fsync 所有文件；
  5. fsync generation 目录；
  6. 原子更新 CURRENT；
  7. fsync session 目录。

  恢复时只读取 CURRENT，忽略未提交临时 generation。

  ### 3.4-D：损坏恢复

  启动时：

  - 验证 manifest schema；
  - 验证 snapshot digest；
  - 扫描 JSONL 到最后有效 offset；
  - 损坏尾部移动到 quarantine；
  - 产生脱敏 audit warning；
  - 不自动恢复未提交 attempt。

  ### 3.4-E：权限和脱敏

  - 新目录默认 0700；
  - 新文件默认 0600；
  - Authorization、token、secret、password、API key 统一脱敏；
  - blob 只保存允许持久化的内容；
  - manifest 不记录原始 prompt。

  ### 3.4-F：GC 与重建

  实现：

  - expired summary 清理；
  - 未引用 blob 清理；
  - 未引用 generation 清理；
  - index sidecar 缺失时从 committed records 重建；
  - GC 操作记录 generation 和 audit。

  ### 3.4-G：故障注入测试

  覆盖崩溃窗口：

  - JSONL 写一半；
  - snapshot 完成但 manifest 未完成；
  - manifest 完成但 CURRENT 未更新；
  - CURRENT 指向损坏 generation；
  - index 缺失；
  - digest 不匹配；
  - 两进程竞争提交。

  ### WP3.4 验收

  - 任一写入阶段崩溃都不会污染上一 committed revision。
  - 重启只恢复 committed snapshot。
  - session/tenant 数据不可串读。
  - 损坏数据可检测、隔离和重建。
  - 文件权限和 redaction 测试通过。

  ———

  # WP3.5：动态 MCP 与 Skills 生命周期

  ## 当前问题

  Skills 生命周期相对成熟，主要风险集中在新增 MCP registry。

  ### 重启恢复后无法重新激活

  当前 load() 恢复 Staged 条目，但 client 为空。

  结果：

  - activate() healthcheck 失败；
  - 再次 stage() 又因为同 ID 已存在而失败；
  - 恢复后的条目形成不可激活状态。

  ### signature 没有完整恢复

  快照保存 signature，但当前 load() 没有完整反序列化并重新验证签名。

  ### 签名绑定范围不足

  现在主要比较：

  - subject_digest
  - source_uri
  - subject_kind

  但 id、version、permissions 和 transport descriptor 没有全部包含在 canonical digest 中。攻击者可能复用已签名 digest，修改权限
  字段。

  ### 默认策略不是 fail-closed

  require_signature_ 默认是 false。对于动态外部 MCP，生产配置应默认要求签名，只允许测试显式关闭。

  ### lease 存在生命周期风险

  当前 lease 释放回调捕获 registry 的裸 this。如果 lease 生命周期长于 registry，会形成悬空访问风险。

  ### 持久化不够可靠

  - 临时文件 + rename，但没有 fsync。
  - 没有跨进程锁。
  - 没有备份 generation。
  - 损坏快照没有回退。
  - parent path 为空时需特殊处理。
  - duplicate entry 恢复未严格报错。
  - registry generation/revision 未定义。

  ### 动态加载策略不足

  还缺少：

  - HTTP/stdio transport descriptor；
  - stdio command allowlist；
  - 可执行文件 digest；
  - 环境变量 allowlist；
  - SSRF/URL policy；
  - tenant visibility；
  - session revision pinning；
  - operator/approval audit；
  - permission diff；
  - drain timeout；
  - failed activation rollback。

  ## 修复计划

  ### 3.5-A：分离持久状态与运行状态

  定义：

  struct McpCapabilityRecord {
      CapabilityManifest manifest;
      CapabilityLifecycleState desired_state;
      uint64_t revision;
      SignatureEnvelope signature;
      TransportDescriptor transport;
  };

  struct McpCapabilityRuntime {
      std::shared_ptr<MCPClient> client;
      size_t active_leases;
      CapabilityLifecycleState runtime_state;
  };

  持久化只保存 Record，不保存连接对象。

  ### 3.5-B：增加恢复后的 rebind

  新增：

  void rebind(
      const std::string& id,
      std::shared_ptr<MCPClient> client);

  或者更安全地：

  void rehydrate(
      const std::string& id,
      McpClientFactory factory);

  重启流程：

  load metadata
  → verify schema/digest/signature/trust
  → state=Validated
  → factory creates client
  → healthcheck
  → state=Staged
  → explicit activate

  绝不因重启自动暴露工具。

  ### 3.5-C：canonical manifest digest

  canonical JSON 至少包含：

  - ID；
  - version；
  - kind；
  - origin；
  - transport descriptor；
  - permissions；
  - executable digest；
  - environment allowlist；
  - tenant visibility；
  - dependency lock。

  对 canonical JSON 计算 SHA-256，签名必须绑定该 digest。

  ### 3.5-D：修复签名和来源验证

  恢复时必须：

  1. 反序列化 signature；
  2. 验证 Ed25519；
  3. 验证 key ID；
  4. 验证 publisher；
  5. 验证 source prefix；
  6. 验证有效期；
  7. 验证 revoked key/publisher/capability；
  8. 验证 canonical manifest digest；
  9. 验证 trust store revision。

  建议为 MCP 增加独立的 Capability trust role，而不是长期复用 Skills 的 Package 角色。

  ### 3.5-E：默认 fail-closed

  生产路径：

  require_signature = true;

  仅测试 fixture 或显式 development 配置允许 unsigned manifest，并必须写入 audit：

  {
    "outcome": "legacy_unsigned_allowed",
    "environment": "development"
  }

  ### 3.5-F：修复 lease 所有权

  把 registry 内部状态放入：

  std::shared_ptr<RegistryState>

  lease 只持有：

  std::weak_ptr<RegistryState>

  释放时先 lock()，避免 registry 已销毁后的悬空访问。

  ### 3.5-G：可靠持久化

  采用 generation 模式：

  registry/
  ├── CURRENT
  ├── generations/
  │   ├── 000001.json
  │   └── 000002.json
  └── lock

  提交：

  - 文件锁；
  - 写 generation；
  - SHA-256；
  - fsync；
  - 原子更新 CURRENT；
  - fsync 目录。

  恢复：

  - CURRENT 损坏时回退上一有效 generation；
  - 所有记录重新验证签名；
  - Active 一律降级为 Validated/Staged；
  - Removed tombstone 保留到 GC。

  ### 3.5-H：安全 transport descriptor

  HTTP MCP：

  - 只允许 HTTPS，开发模式除外；
  - SSRF 检查；
  - host allowlist；
  - 禁止凭证出现在 URL；
  - header secret 仅引用 CredentialStore key。

  stdio MCP：

  - command 必须是绝对路径或受信程序 ID；
  - executable SHA-256 锁定；
  - 参数无 shell 展开；
  - 禁止 $(), backticks 和 shell wrapper；
  - environment 仅传 allowlist；
  - cwd 必须在受控目录；
  - 限制进程资源和超时。

  ### 3.5-I：事务和回滚

  状态机扩展为：

  Discovered
  → Validated
  → Staged
  → Active
  → Draining
  → Removed

  任一步失败
  → Failed
  → rollback previous committed revision

  activate() 必须保证：

  - 所有工具名称先校验；
  - 权限差异先审批；
  - ToolBus 原子发布；
  - 发布失败不留下部分工具；
  - registry commit 与 audit 顺序明确。

  ### 3.5-J：session revision pinning

  每个 session 记录：

  capability_id
  capability_version
  capability_revision
  manifest_digest

  升级后：

  - 老 session 继续使用旧 revision；
  - 新 session 使用新 revision；
  - draining revision 不接受新 lease；
  - 已有 lease 完成后才可回收。

  ### 3.5-K：测试矩阵

  必须新增：

  - 有效 Ed25519 签名；
  - 签名被篡改；
  - permission 被篡改；
  - origin 被替换；
  - revoked key；
  - expired key；
  - unsigned production manifest；
  - 恶意 stdio command；
  - SSRF URL；
  - snapshot 截断；
  - CURRENT 损坏；
  - 重启 rebind/activate；
  - drain 期间拒绝新 lease；
  - registry 先销毁、lease 后销毁；
  - session 固定旧 revision；
  - activation 中途失败回滚；
  - 跨 tenant 不可见。

  ### WP3.5 验收

  - 重启后能安全 rebind，但不会自动 activate。
  - 所有生产动态 MCP 都经过签名、来源和权限验证。
  - 修改 manifest 任意安全字段都会导致签名失败。
  - drain 不影响已有 lease，但拒绝新 lease。
  - registry 崩溃恢复不产生半激活服务。
  - 两个 session 可以分别固定旧、新 revision。

  ———

  # 推荐执行顺序

  建议拆成以下七个可独立验收的修复批次：

  1. P3-FIX-0：WP3.0 文档事实矩阵和 CTest 标签。
  2. P3-FIX-1：Faiss 编译隔离、sidecar 和后端契约。
  3. P3-FIX-2：统一 Memory Assembly 输入与事件。
  4. P3-FIX-3：MemoryCompactorRegistry 和隔离子 LLM。
  5. P3-FIX-4A：MemoryStore session API、目录隔离和 schema。
  6. P3-FIX-4B：原子提交、崩溃恢复、redaction 和 GC。
  7. P3-FIX-5：MCP canonical signature、rehydrate、持久化 generation、lease 安全和 session pinning。

  依赖关系：

  WP3.0
    └─ WP3.1
        └─ WP3.2
            ├─ WP3.3
            └─ WP3.4
                └─ WP3.5

  最终关闭条件不是“测试存在”，而是：

  代码实现
  + 单元契约
  + 故障注入
  + 跨组件集成
  + 文档证据
  + 默认离线 CI
  = [x] 集成完成
