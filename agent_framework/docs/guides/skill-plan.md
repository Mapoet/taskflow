# Agent Framework Skills 完整化升级计划

## 1. 目标与判定口径

本文以 `skills-complete.md` 的能力分类为基线，审阅 Agent Framework 当前对 Skill 的支持，
并规划从本地渐进式加载器升级为可声明、可授权、可组合、可测试、可升级的 Skill 平台。

完整 Skill 定义为：

> Skill = Metadata + Capability + Knowledge + Execution + Validation

审阅使用三种状态：

- **已集成**：资源由 Skill manifest 声明，并受 Registry、Loader、Runtime 或 Policy 控制。
- **框架已有但未集成**：框架存在通用模块，但 Skill 不能声明、绑定、授权或管理它。
- **缺失**：Skill 层和通用框架均无完整实现。

因此，框架拥有 MCP、ToolBus、PromptRenderer 或 GraphExecutor，并不等于 Skill 已支持 MCP、
Tool、Prompt 或 Workflow。

## 2. 当前能力矩阵

| 能力 | 状态 | 当前实现 | 主要缺口 |
| --- | --- | --- | --- |
| Metadata | 部分集成 | `SkillIndexEntry`、Frontmatter parser、diagnostics | 无正式 schema、作者、来源、依赖、兼容范围和权限模型 |
| Registry | 基础集成 | 多根扫描、重复 ID 检查、关键词路由 | 无版本解析、启停、安装、升级、快照和来源追踪 |
| Instructions | 已集成 | 正文按需读取、字符预算、时间戳缓存 | 无内容哈希、版本隔离和原子缓存失效 |
| Reference | 部分集成 | 类型化读取、jail、大小限制 | 无 MIME、分页、索引、检索和 citation |
| Script | 基础集成 | allowlist、参数、最小环境、超时、输出限制、取消 | 无每脚本 schema、CPU/内存配额、进程组清理 |
| CLI resource | 部分集成 | 可声明和受控读取 | 无独立执行契约、可执行发现和参数/结果 schema |
| Tool | 框架已有但未集成 | `ToolBus` | `allowed_tools` 仅解析，未运行时强制；无私有/导出 Tool |
| MCP | 框架已有但未集成 | `MCPClient`、`MCPTool` | 无 Skill 服务声明、凭据引用、过滤、生命周期和授权 |
| Template | 框架已有但未集成 | Prompt/Workflow template API | 无类型化资源、变量 schema 和 Skill 作用域 |
| Schema | 通用能力有限 | Tool 参数 schema validation | 无 Skill 输入输出及资源间 schema 契约 |
| Prompt | 框架已有但未集成 | `PromptRenderer` | 无角色 Prompt 声明、变量授权、组合和版本绑定 |
| Workflow | 框架已有但未集成 | WorkflowBuilder、GraphExecutor | 无 Skill DAG 声明、输入输出映射、循环和子任务契约 |
| Config | 缺失 | 零散环境变量 | 无类型配置、默认值、覆盖层和 secret reference |
| Asset | 缺失 | 无 Skill 专用管理 | 无 MIME、哈希、配额、只读映射和流式加载 |
| Model | 框架部分已有但未集成 | LLM/model adapters | 无本地模型资源、设备、内存、runtime 和校验策略 |
| Tests | 缺失 Skill 契约 | 框架自身有 CTest | 包内测试不可发现、隔离执行和报告 |
| Lifecycle | 缺失 | `scan_or_reload` | 无 install/enable/disable/update/remove/pin/rollback |
| Supply chain | 已集成 | 确定性 `.tfskill`、Ed25519、SBOM/provenance、trust store、签名 Registry | 暂无透明日志、阈值签名、TUF/Sigstore 兼容和自动密钥轮换 |
| CLI management | 部分集成 | `skillctl list/validate/show/read` | 无 lint/test/package/install/doctor/permissions/graph |
| Observability | 未统一 | 零散日志和 Agent event | 无 Skill 资源、权限、依赖和调用审计事件 |

当前状态应定义为 **core-supported / resource-incomplete**：L1 元数据、L2 指令和部分 L3
执行已可用，但尚不是完整的 Skill 平台。

## 3. 目标架构

```text
SkillPackage
  -> ManifestParser + ManifestValidator
  -> DependencyResolver + SkillLock
  -> SkillRegistrySnapshot
  -> SkillResourceStore
  -> SkillPolicyEngine
  -> SkillRuntime
       -> Tool / MCP binding
       -> Script / CLI executor
       -> Prompt / Template loader
       -> Workflow factory
       -> Reference / Asset / Model reader
  -> SkillTestRunner
  -> SkillLifecycleManager
  -> SkillEventSink + Audit
```

### 3.1 Manifest v1

`SKILL.md` Frontmatter 保持默认事实源。可生成规范化 JSON 用于工具链，但禁止维护两份独立
manifest。建议结构：

```yaml
api-version: agent.taskflow/v1
kind: Skill
name: gnss-ro-qc
version: 1.2.0
description: GNSS RO profile quality control
license: Apache-2.0
authors: [Naifeng Fu]
compatibility:
  agent-framework: ">=2.0 <3.0"
dependencies:
  - name: netcdf-reader
    version: "^1.1"
permissions:
  tools: [read_file]
  network: [https://data.example.org]
  env: [ROPP_HOME]
resources:
  scripts:
    - id: qc
      path: scripts/qc.py
      input-schema: schemas/qc-input.json
      output-schema: schemas/qc-output.json
  references:
    - id: algorithm
      path: references/algorithm.md
  workflows:
    - id: full-qc
      path: workflows/qc.yaml
tests:
  - path: tests/smoke.yaml
```

### 3.2 类型化资源

统一 `SkillResourceDescriptor` 至少包含：

- `id`、`kind`、`path`、`media_type`；
- `sha256`、`size_limit`、`optional`；
- `input_schema`、`output_schema`；
- `permissions`、`runtime`、`executable`；
- `cache_policy` 和依赖资源引用。

Script、CLI、Reference、Tool、MCP、Template、Schema、Prompt、Workflow、Config、Asset、
Model、Test 必须走同一条相对路径、canonical jail、普通文件、大小和哈希验证链。

### 3.3 Runtime 与 Policy

`SkillRuntime` 负责装配能力，`SkillPolicyEngine` 负责授权。资源存在不代表允许执行。
运行上下文必须包含：

- skill id、version、package digest、registry snapshot；
- task/session/trace/attempt/iteration/depth；
- deadline、cancellation callback、event sink；
- tool/network/filesystem/environment/secret grants；
- resource/output/CPU/memory budgets。

## 4. 分阶段实施

## Stage 0：冻结并验证当前基线，P0

### 完成证据（2026-07-13）

- 完整 Debug 构建成功，Taskflow、Workflow、Agent Framework、示例及测试目标均构建到 100%。
- Workflow/Agent/MCP/Skills 垂直集合 12/12 通过。
- 全量 `ctest -j8 --output-on-failure` 2995/2995 通过，总耗时 153 秒。
- `agent_loop_tool_parallel_wp21b` 已由脆弱的 115 ms 墙钟阈值改为 `max_active >= 2`
  直接并发重叠断言，并连续运行 20 次通过。
- 新增 Tool/MCP cancellation、Skills metadata/resource/script/CLI 直接正负向测试及
  `skill-unit`、`skill-runtime-integration`、`skill-policy-security` 标签。

### 工作项

1. 完成当前未提交 Phase 2 修改的完整编译和全量 CTest。
2. 补充 Tool/MCP cancellation、Skills metadata/resource/script/CLI 的直接测试。
3. 固化 legacy 行为：未声明资源列表时，仅允许对应兼容目录。
4. 记录编译器、配置、目标数量、测试总量和已知跳过项。
5. 独立提交稳定基线后再变更 Manifest 公共 API。

### 退出标准

- 完整构建成功且无新增回归。
- 取消、路径 jail、资源类型和 CLI 有直接正负向测试。
- 当前兼容语义形成 fixture，而不是依赖实现细节。

## Stage 1：Manifest v1 与完整资源类型，P0

### 完成证据（2026-07-13）

- 新增 `SkillManifest`、`SkillDependency`、`SkillPermissionSet` 和统一资源描述符，覆盖
  Script、CLI、Reference、Tool、MCP、Template、Schema、Prompt、Workflow、Config、
  Asset、Model、Test 13 类资源。
- `SKILL.md` 支持 `agent.taskflow/v1`/`Skill`；legacy frontmatter 归一化为
  `agent.taskflow/v0`，未知普通字段保留并告警，未知资源类型确定性报错。
- Registry 发布前校验 ID、SemVer、引用、存在性、canonical jail、普通文件、大小和
  SHA-256；无效 Skill 仅保留结构化 diagnostics，不进入可执行快照。
- v1 资源严格按声明授权；legacy 仅在对应资源列表为空时保留 `scripts/`、
  `references/`、`cli/` 目录兼容回退。
- `skillctl inspect <id> --resolved` 输出不含 secret 值的规范化 manifest；schema 随
  Agent Framework 安装。
- 完整构建成功；全量 CTest 2997/2997 通过，0 失败，总耗时 142.33 秒。
- 无 OpenSSL 配置下本阶段四个 Skill 对象可独立编译；完整无 OpenSSL 构建仍被既有
  `web_search_ddg.cpp` 对 `httplib::SSLClient` 的无条件引用阻断，不属于本阶段回归。

### 实施状态

**已完成。** Stage 2 的 schema 运行时校验和权限求交未提前实现，保持阶段边界。

### 文件范围

新增：

- `include/agent/skill_manifest.hpp`
- `include/agent/skill_resource.hpp`
- `src/skills/skill_manifest.cpp`
- `src/skills/skill_manifest_validate.cpp`
- `schemas/skill-manifest-v1.schema.json`

修改：

- `skill_types.hpp`
- `skill_frontmatter_parse.cpp`
- `skill_registry.cpp`
- `skill_loader.cpp`
- `skillctl.cpp`

### 步骤

1. 定义 `SkillManifest`、`SkillDependency`、`SkillPermissionSet` 和资源描述符。
2. 增加 `api-version`/`kind`；旧格式解析为 legacy v0，再归一化为内存 v1。
3. 覆盖 `skills-complete.md` 全部资源类型。
4. 验证资源 ID 唯一性、跨资源引用、文件存在性、哈希和版本格式。
5. 未知普通字段保留并 warning；未知资源 kind、危险路径和无效 schema 报 error。
6. 错误 Skill 不进入可执行快照，但保留结构化 diagnostics。
7. `skillctl inspect --resolved` 输出归一化 manifest，不输出 secret 值。

### 兼容边界

- legacy `scripts/references/cli` 支持一个大版本周期。
- v1 显式声明某类资源后，该类禁止回退到目录隐式授权。
- 现有字段的行为保持稳定，废弃项输出机器可读 warning。

### 退出标准

- 13 类资源都可表达、解析、验证和定位。
- 任一资源不能绕过统一安全检查。
- legacy 与 v1 fixtures 同时通过。

## Stage 2：Schema 与权限强制，P0

### 完成证据（2026-07-13）

- 新增通用 JSON schema 运行时校验，Script、CLI、MCP、Workflow 使用相同的
  `SkillRuntime::begin/finish` 契约；错误包含 instance path、schema path 和 schema
  resource location。
- 新增 `SkillPolicyEngine`，对 Tool、Network、Environment、Filesystem Read/Write、
  Secret 执行 manifest request 与 task grant 求交，六类权限均有允许和拒绝测试。
- Agent Loop 只导出授权工具；ToolBus 在 hook 参数改写前后执行 request-scoped
  authorization。内建 Web/Filesystem Tool 通过 `ToolMeta.permission_targets` 约束真实
  origin/path，伪造 ToolCall 无法绕过。
- v1 Script/CLI/Resource 访问绑定 active Skill 请求上下文；Linux Script/CLI 使用
  `unshare` network namespace 与 `bwrap` 文件系统沙箱，包默认只读、写目录最小挂载、
  HOME 隐藏、网络关闭、环境白名单、secret reference 文件注入和输出脱敏。
- Skill event 统一覆盖 started/completed、permission denied、schema invalid、budget、
  cancel 和 timeout；运行中子进程取消也会回传 `Cancelled` 事件，稳定失败码已固化为
  公共常量。
- Stage 2 垂直 CTest 8/8 通过，覆盖 unit、schema contract、runtime integration、
  policy security、Agent Loop integration、真实进程沙箱和 legacy v0 兼容。
- 完整构建成功；最终全量 CTest 3000/3000 通过，0 失败，总耗时 275.33 秒。收口前一次
  全量 CTest 同样 3000/3000 通过，总耗时 278.88 秒。

### 实施边界

**已完成。** MCP 与 Workflow 在本阶段共享
`TaskControl`、Policy、schema 和 event sink guard contract；MCP server 生命周期、Tool
命名空间与过滤属于 Stage 3，Workflow DSL、循环、subflow/submodule、restart/resume
属于 Stage 4，本阶段不提前声明这些执行能力完成。

### 步骤

1. 为 Skill、Script、CLI、Tool、Workflow 定义 input/output schema 引用。
2. 调用前校验输入，调用后校验输出；错误包含 JSON path 和 schema location。
3. 新增 `SkillPolicyEngine`，将任务 grant 与 manifest permission 求交集。
4. 将 `allowed_tools` 从提示信息升级为 ToolBus 运行时强制授权。
5. Network host、环境变量、文件写范围和 Secret 默认拒绝。
6. Secret 仅通过引用注入，不进入 manifest、日志、Prompt 或 CLI JSON。
7. 权限、预算、取消、超时、schema 失败进入统一 Skill event。

### 稳定失败码

- `skill_permission_denied`
- `skill_input_invalid`
- `skill_output_invalid`
- `skill_resource_budget_exceeded`
- `skill_cancelled`
- `skill_dependency_unavailable`

### 退出标准

- 每项权限至少一个允许和一个拒绝测试。
- Script、CLI、MCP、Workflow 共用 TaskControl、Policy 和 event sink。
- 未声明能力无法通过直接 ToolBus 调用绕过。

## Stage 3：Tool、MCP、Prompt、Template 集成，P0/P1

### 完成证据（2026-07-13）

- 新增 `SkillCapabilityRuntime` 与 task-pinned `SkillCapabilityBinding`，所有能力使用
  `skill::<skill-id>::<capability-id>` 命名空间；ToolBus 先验证完整注册集合，再在单锁内
  原子发布，冲突返回 `skill_capability_conflict` 且不破坏已有 owner。
- Tool descriptor 可导入现有 Tool，并以 `export` 明确控制是否进入 LLM Tool 列表；私有
  Tool 仍可由已授权的 Skill 按完整名称调用。
- MCP descriptor 支持 stdio/http/mock transport、Eager/Lazy startup、tool filters、显式
  tools 与 secret references。每个 MCP resource 共享一个 session，关闭 binding 时先撤销
  新调用，再传播 TaskControl cancel、有界等待并断开连接；HTTP 创建前强制 network grant。
- Prompt/Template descriptor 在绑定时校验并固定快照，只允许 input/context/task 三类变量
  来源，支持变量 schema、JSON Pointer、required 与输出字节上限；环境和 Secret 来源默认拒绝。
- `SkillRuntime::begin_snapshot` 与 `SkillLoader::load_resource_snapshot` 使运行中任务在 Registry
  disable/reload 后仍使用固定 entry、manifest、descriptor 和 schema；新绑定立即失败。
- 新增 `skill_capability_runtime_contract`，直接覆盖 LocalTool/MCP、私有导出、过滤、Secret、
  网络拒绝、Prompt 缺失/越权/超限/类型错误、原子冲突、Lazy session 复用、禁用后快照和取消。
- 完整 Debug 构建成功；Stage 3 与关联 Skills/ToolBus/MCP/Prompt 测试 15/15 通过；最终全量
  CTest 3001/3001 通过，0 失败，总耗时 280.30 秒。

### 实施状态

**已完成。** 本阶段不包含 Workflow DSL、循环、subflow/submodule、retry/restart/resume；
这些能力仍严格属于 Stage 4。

### 步骤

1. 定义命名空间 `skill::<skill-id>::<capability-id>`，禁止静默覆盖。
2. 支持导入现有 Tool、声明 Skill 私有 Tool 和显式导出 Tool。
3. MCP descriptor 声明 server、transport、tool filters、secret references 和启动策略。
4. MCP 连接归属 Skill Runtime 生命周期，关闭、取消和超时必须有界且可观测。
5. Prompt/Template 声明变量 schema、允许的上下文来源和最大字节数。
6. Prompt 不得隐式读取环境、secret 或未授权资源。
7. 禁用 Skill 后新任务不可获得绑定；运行中任务使用固定快照完成或被取消。

### 退出标准

- 单个 Skill 可受控调用 LocalTool 和 MCP Tool。
- Prompt/Template 缺变量、越权和超限均确定性失败。
- 同名能力冲突有稳定诊断，加载顺序不改变结果。

## Stage 4：Workflow、循环与 Subtask 集成，P1

**状态（2026-07-13）：已实现。** 实施记录见
`docs/superpowers/plans/2026-07-13-skill-workflow-stage4.md`。Stage 4 固定的是单次运行的
manifest、Workflow descriptor 和精确依赖版本；持久化 lockfile、内容寻址安装与崩溃恢复仍属于 Stage 5。

### 步骤

1. 定义版本化 Skill Workflow DSL，映射现有 WorkflowBuilder/GraphExecutor。
2. 定义 Skill input -> ValueMap 和 Workflow result -> Skill output 的类型映射。
3. Workflow 只通过资源 ID 引用同包资源或带版本依赖 Skill。
4. 循环、迭代、subflow/submodule 使用 Taskflow 原生控制流和统一 TaskControl。
5. 子任务继承 deadline、cancel、trace、depth、permissions、budget，且只能收紧权限。
6. 固化 retry/restart/resume 语义；副作用节点要求 idempotency key 或禁止重放。
7. 每次运行固定 Registry snapshot 和 dependency lock，禁止循环中途切换版本。

### 退出标准

- Workflow 多次循环、重启、嵌套后结果和资源版本稳定。
- Local subflow 与 Remote A2A child 使用统一结果、错误和事件协议。
- Cancel 后不启动下一迭代，不提交失败 attempt 或重复副作用。

### 完成证据

- `SkillWorkflowRuntime` 支持 `agent.taskflow/workflow/v1` 的 `tool`、`workflow`、`loop` 和 `child` 节点。
- `loop` 映射到 `GraphBuilder::create_loop`；嵌套 Workflow 映射到 `create_subtask_module`，输入输出只允许显式 JSON Pointer。
- task-scoped capability binding 不发布全局 ToolBus 名称，同一 Skill 可并发绑定。
- Start/Retry/Restart/Resume 使用兼容性检查点；Restart 保留幂等账本，Write/Unknown 与 child 重放缺少 idempotency key 时拒绝。
- ChildTask metadata 和结果协议包含 mode、checkpoint、permissions、budget、usage、events 与稳定 error code；A2A resume 作为显式新任务发送给对端。
- `skill_workflow_stage4` 覆盖多次循环、嵌套模块、循环边界恢复、重启、取消、权限收窄、副作用去重和运行中资源快照稳定性。
- 功能、模块和集成标签测试全部通过；Stage 4 专项测试连续运行 20 次通过；全量构建成功且 CTest 3002/3002 通过（0 失败，228.94 秒）。

## Stage 5：生命周期、依赖和原子快照，P1

### 建议 API

```text
SkillLifecycleManager::install/enable/disable/update/remove/rollback
SkillRegistry::snapshot
SkillDependencyResolver::resolve
```

### 步骤

1. 使用 SemVer 解析版本和兼容范围，禁止字符串比较版本。
2. 生成 `skills.lock`：精确版本、来源 URI、package digest、依赖图、签名身份。
3. 安装到 content-addressed store，验证成功后原子发布 Registry snapshot。
4. 任务启动时 pin snapshot；运行中更新不影响既有任务。
5. Disable 只影响新任务；存在引用时 Remove 拒绝或延迟执行。
6. Reload 先构造完整新快照，失败时继续使用旧快照。
7. 缓存键使用 package digest + resource digest，不只使用文件时间戳。

### 退出标准

- 并发 read/reload/update 不暴露半更新状态。
- 依赖冲突输出最小冲突集和可操作建议。
- Lockfile 可重现相同依赖图和资源 digest。

### 完成证据

- `SkillLifecycleManager` 已实现 install、enable、disable、update、remove、rollback 和 reload；安装包进入 SHA-256 content-addressed store，残留 transaction 在恢复时清理。
- SemVer 与范围解析不使用字符串排序；依赖解析按版本优先、package digest 次序确定性选包，并返回最小冲突约束与修复建议。
- `agent.taskflow/skills-lock/v1` 固化 roots、精确版本、来源 URI、package/resource digest、签名身份和依赖边；恢复时逐项复核内容、manifest、资源摘要与来源身份。
- Registry 使用不可变 generation snapshot 原子发布；失败 reload/持久化提交保留旧 generation。Workflow、capability 与 runtime ticket 均固定任务启动时的 entry、manifest 和 package lease。
- Disable 仅发布给新任务；旧 snapshot 持有 lease 时 Remove 返回 `skill_package_in_use`。Loader 的受管资源缓存键使用 package digest + resource digest。
- `skill_lifecycle_stage5` 覆盖 SemVer 负向契约、确定性解析、最小冲突集、循环依赖、锁文件损坏/来源篡改、失败提交、并发读更新、运行中升级、回滚、引用保护和崩溃恢复；连续运行 20 次通过。
- Stage 1-5 技能标签回归 10/10 通过，完整 Debug 目标集构建成功。沙箱内全量 CTest 为 2985/3001 通过；其余 16 项均是既有 HTTP/A2A/在线模型测试无法绑定本地端口或发起传输，沙箱外复跑未获授权。

## Stage 6：正式 CLI、测试运行器和 CI，P1

**状态（2026-07-15）：已完成。**

### CLI 范围

```text
skillctl list/show/validate/inspect/read
skillctl lint/test/graph/permissions/doctor
skillctl package/install/update/enable/disable/remove
```

### 步骤

1. 将示例 `skillctl` 升级为正式安装目标和稳定退出码契约。
2. `validate` 检查规范正确性；`lint` 检查风格和最佳实践。
3. `test` 发现 manifest tests，在临时 jail 和最小环境执行。
4. Fixture 支持输入、预期输出/错误、事件序列和资源 digest。
5. `doctor` 检查解释器、外部 CLI、MCP transport、模型 runtime 和权限依赖。
6. 所有命令提供稳定 JSON 输出，供 CI 和管理器消费。
7. 包内测试失败必须阻止 package/install。

### 退出标准

- 不启动 Agent Server 即可完成 lint、validate、test 和 doctor。
- CLI 行为、JSON schema 和退出码有 contract tests。
- CI 包含 unit、integration、security-negative 和 package reproducibility。

### 完成证据

- `skillctl` 已成为正式安装目标；除 `read --raw` 外统一输出 `agent.taskflow/skillctl-output/v1`，退出码 0/2/3/4/5/6/64/70 有直接契约测试。
- 已实现 list/show/validate/inspect/read、lint/test/graph/permissions/doctor、package/install/update/enable/disable/rollback/remove；lifecycle 输出 roots、rootRanges、精确 packages、package/resource digests 与 Registry generation。
- `SkillTestRunner` 覆盖 resource/tool/workflow/script/CLI，比较 output/error/events/stdio/exit/digests，并以固定 snapshot、临时 jail、空环境、无 Secret、声明 mock、timeout/cancel 隔离运行；专项测试连续 20 次通过。
- `SkillPackageGate` 在写入前执行验证、lint、包内测试与重复身份检查；测试证明失败前后 store 文件、lock/history 和 Registry generation 不变，并拒绝 traversal、symlink、FIFO、缺失资源与摘要不匹配。
- Ubuntu CI 新增无凭据、无网络变量的 `skill-stage6-offline` job；全新 `build-stage6-ci` 本地复现中四标签并集 5/5 通过。
- `skill-*` 标签集 16/16 通过，并逐项连续运行 20 次（共 320 次执行）无失败；完整 Debug 目标集构建到 100%。
- 安装到隔离前缀后，`skillctl` 可直接完成 validate/package；Manifest、Test 与 CLI output 三个 v1 schema 均随安装产物发布。
- 沙箱内全量 CTest 为 2992/3008 通过；其余 16 项中 15 项为既有 HTTP/A2A 测试无法绑定本地监听端口，1 项为在线模型 HTTP 传输不可用，Stage 6 新增测试无失败。
- Stage 6 不定义归档、签名验证或远程 Registry；这些供应链协议仍保留给 Stage 8。

## Stage 7：Reference、Asset 与 Model 管理，P1/P2

### 步骤

1. 依据 MIME/descriptor 选择 text、binary、stream 或 memory-map 读取。
2. Reference 支持分页、结构化 citation、可选索引和检索接口。
3. Asset/Model 声明 digest、大小、license、来源和 runtime requirements。
4. 大资源按需进入 content-addressed cache，支持 quota、LRU 和 pin。
5. 模型声明设备、精度、内存和 runtime；禁止自动执行模型包任意代码。
6. 下载、解压和加载分别限制大小，防止 zip bomb 和路径穿越。

### 退出标准

- 大二进制不会进入 Prompt，也不会默认完整读入内存。
- 离线模式可依据 lockfile/cache 确定资源是否齐备。
- 模型与资产的 digest、license 和来源可审计。

### 完成证据

- `SkillResourceAccess` 固定 package/manifest/cache lease，统一执行 canonical jail、普通文件、大小与 digest 校验，并只暴露调用方限制的 read、stream 或 mmap 窗口。
- Reference 分页保持 UTF-8 边界并返回结构化 citation；确定性派生索引提供有界检索，索引身份绑定 resource digest。
- 内容寻址 cache 实现原子提交、quota/LRU、pin、lease、verify 与 GC；Artifact 导入对下载、展开大小、文件数、压缩比、路径、类型和取消清理分别 fail closed。
- Model admission 检查 runtime、device、precision、memory、mmap/open 与 cache 可用性，保持只读且 `automaticExecution=false`。
- `skillctl reference/cache/model` 与扩展 doctor 提供稳定 JSON 审计面；Stage 7 CI 使用无凭据、清空代理变量的离线 job 和四个专项标签。
- 稀疏大文件回归 fixture 明确验证 materialization、stream chunk 和 mmap 均受请求窗口约束。
- 全新 `build-stage7` 完整 Debug 构建到 100%；四标签并集 7/7 通过，7 个专项测试各连续运行 20 次（共 140 次执行）无失败。
- 隔离安装前缀包含 `skillctl`、Stage 7 公共头文件与三个 v1 schema；沙箱内全量 CTest 为 2999/3015，通过之外的 16 项均为既有 HTTP/A2A/LLM mock server 无法绑定本地监听端口，Stage 7 新增测试无失败。
- 远程传输、归档格式、密码学签名、可信发布者、SBOM 与远程 Registry 未在本阶段实现，继续由 Stage 8 定义。

## Stage 8：包、签名与远程 Registry，P2

### 完成证据（2026-07-16）

- `.tfskill` 使用规范化 ZIP32，固定排序、时间戳、压缩/权限和元数据；拒绝 traversal、链接、
  特殊文件、重复/大小写冲突与资源预算越界，相同输入逐字节一致。
- 包内嵌 CycloneDX 1.6 SBOM 与来源证明；Ed25519 detached signature 覆盖 package/Registry
  subject、发布者、source scope、SBOM/provenance digest，并执行 key role、有效期和撤销检查。
- 签名 Registry v1 只解析精确版本；镜像和离线导入都固定 size、SHA-256 与包签名，失败先于
  store/lock/history/Registry generation 变更。
- store 与 lock 持久化 archive、publisher、key、signature、SBOM、provenance、Registry identity；
  远程包禁止 unsigned，本地 unsigned 仅显式 opt-in 并标记 `legacyUnsigned`。
- `skillctl package build/inspect/sbom/sign/verify`、`registry sync/resolve` 和 verified archive
  install/update 已进入稳定 JSON/退出码契约；[供应链操作指南](skill-supply-chain.md)记录完整流程。
- 新增五个 Stage 8 CI 标签和无凭据/无真实网络 Ubuntu gate；7 个专项测试各连续运行 20 次，
  ODR 跨模块回归及 Stage 7 Artifact importer 也连续运行 20 次。
- 全新 Debug/C++20/OpenSSL 构建完成；安装前缀验证 `skillctl`、4 个公共头和 5 个 schema；最终
  离线 fixture + loopback 全量 CTest 为 3022/3022 通过，总耗时 206.24 秒。

**实施状态：已完成。** 当前状态达到 `platform-complete`；下列透明日志、多方签名等仍是明确
非目标，不影响 Stage 8 退出标准。

### 步骤

1. 定义确定性包格式：路径排序、时间戳归一化、禁止链接和设备文件。
2. Package digest 覆盖 manifest 和全部声明资源。
3. 支持签名、可信发布者、撤销列表和来源证明。
4. 生成最小 SBOM，列出脚本 runtime、CLI、模型和依赖 Skill。
5. 远程 Registry 只提供索引和不可变包；本地 Policy 决定安装与启用。
6. 支持镜像、离线导入和 digest pin；禁止仅凭可变 tag 执行。

### 退出标准

- 篡改包、未知签名、digest 不符和撤销发布者均被拒绝。
- 相同 lockfile 在受支持平台解析为相同 Skill 图。

## 5. 测试矩阵

### 5.1 单元测试

- Manifest v0/v1 解析、归一化、未知字段和诊断位置。
- SemVer、冲突、循环依赖和可选依赖。
- 13 类资源、路径穿越、symlink、特殊文件和大小限制。
- Input/output schema 正向、负向和边界测试。
- Tool/network/env/filesystem/secret Policy 交集。
- Digest、cache key、lockfile 和确定性 package。
- Cancellation、deadline、进程组清理和输出截断。

### 5.2 集成测试

- Skill -> LocalTool -> schema output。
- Skill -> MCP HTTP/stdio -> cancel/timeout/disconnect。
- Skill -> Prompt/Template -> mock LLM。
- Skill -> Workflow -> loop -> subflow -> result mapping。
- Skill A -> versioned Skill B -> lockfile resolution。
- Install -> enable -> run -> update -> old/new snapshot isolation。

### 5.3 综合测试

```text
install signed package
-> resolve dependencies and write lock
-> start Agent task with pinned snapshot
-> load reference and prompt
-> execute three workflow iterations
-> call LocalTool and MCP child
-> execute nested subflow
-> validate output schema
-> emit events and audit records
-> update Skill concurrently
-> current task remains on old version
-> next task uses new version
-> cancel child and verify no next iteration/session commit
```

### 5.4 安全测试

- `../`、绝对路径、symlink、hardlink、FIFO、device file。
- Zip bomb、超大 manifest/output、参数注入和环境泄露。
- 未声明 Tool/MCP/host/env/secret 访问。
- Prompt/Template 请求越权上下文。
- 包篡改、依赖替换、签名错误和 rollback attack。
- Disable/Remove 与并发执行竞态。

### 5.5 性能与稳定性

- 1/100/1000/10000 Skills 的扫描、解析、路由和快照发布时间。
- 并发任务持有旧 snapshot 时 update/reload 的延迟和内存。
- 大 Reference/Model 流式读取与 cache 命中率。
- 1000 次循环、取消、重启后无进程、FD、MCP 连接和缓存泄漏。

## 6. CI 门禁

建议标签：

- `skill-unit`
- `skill-manifest-contract`
- `skill-policy-security`
- `skill-runtime-integration`
- `skill-workflow-e2e`
- `skill-lifecycle-e2e`
- `skill-package-reproducibility`
- `skill-registry-supply-chain`
- `skill-package-archive`
- `skill-signature-security`
- `skill-sbom-contract`
- `skill-supply-chain-e2e`
- `skill-resource-management`
- `skill-cache-security`
- `skill-reference-retrieval`
- `skill-model-admission`

每项权限和资源控制至少有一个拒绝测试；每项生命周期操作至少有一个并发或失败恢复测试。
P0/P1 工作不得仅增加文档或 happy-path 测试。

## 7. 完成定义

- [x] `skills-complete.md` 的 13 类资源均有类型化 descriptor 和验证器。
- [x] 可执行资源共用 TaskControl、Policy、预算、event sink 和 audit context。
- [x] Tool、network、filesystem、env、secret 默认拒绝并强制执行。
- [x] Skill、Script、CLI、Tool、Workflow 输入输出均支持 schema。
- [x] Workflow 循环、迭代、subflow/submodule、重启、嵌套使用固定版本快照。
- [x] Install/update/rollback/remove、SemVer dependency 和 lockfile 闭环通过。
- [x] CLI 覆盖 validate/lint/test/package/install/doctor，JSON 和退出码稳定。
- [x] 包内测试、框架集成、安全负向和综合测试进入 CI。
- [x] 本地资源的 digest、来源、license、cache 状态和模型 admission 可审计。
- [x] 签名、可信发布者、SBOM 和远程 Registry 策略可审计。
- [ ] 1000 次循环/取消/重启压力测试无资源泄漏或重复副作用。

状态门槛：

- Stage 1-4 完成前：`core-supported / resource-incomplete`。
- Stage 5-7 完成后：`runtime-complete / lifecycle-incomplete`。
- Stage 8 和全部门禁完成后：`platform-complete`。

## 8. 明确边界

- 不把 Python 或系统包管理器直接嵌入 Agent 进程。
- Manifest 不保存明文凭据。
- 不承诺跨设备运行模型得到位级一致结果。
- 无签名和 Policy 时不启用远程自动安装。
- 运行任务不得在迭代中途隐式切换 Skill/依赖版本。
- 关键词路由不能替代权限、依赖和 schema 校验。

## 9. 推荐顺序

1. 完成并提交当前 Phase 2 基线。
2. Manifest v1 与类型化资源。
3. Schema 与权限强制。
4. Tool/MCP/Prompt/Template 集成。
5. Workflow/Subtask 集成。
6. 生命周期、依赖和原子快照。
7. CLI/Test/CI。
8. Asset/Model/Reference。
9. Package/Registry/供应链。

禁止提前以远程 Registry 或自动安装绕过 Manifest、Policy、锁文件和测试门禁；否则只会扩大
不受控执行面，而不会提高 Skill 完整性。
