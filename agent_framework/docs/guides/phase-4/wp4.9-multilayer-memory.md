# WP4.9：Multi-layer Memory & Dynamic Context Views

**优先级**：P0  
**逻辑实施位置**：共享契约和 Durable/HITL 基础之后，认知规划与五层验收垂直闭环之前  
**依赖**：WP4.2 Run snapshot/checkpoint、WP4.3 memory write/promotion approval、Phase 3 Memory/RAG/Skill 组件  
**下游**：WP4.0、WP4.1、WP4.5、WP4.6、WP4.7、WP4.8  
**编号说明**：`4.9` 用于保持既有 `4.0–4.8` 任务 ID 稳定，不表示最低优先级。

## 1. 目标与非目标

目标是把当前“会话历史 + RAG + Prompt 装配 + 摘要持久化”升级为具有层级作用域、来源权威、生命周期、访问控制、动态视图、可重放快照和受治理写入的多层记忆系统。

Memory Store 是事实、历史和派生记录的持久层；LLM Prompt 只是针对当前主体、工作流阶段和预算生成的 `MemoryView`。任何压缩、检索或渲染都不得静默改变原始证据、权威指令或 Run 恢复语义。

本工作包不持久化模型私有 chain-of-thought；只保存用户输入、专业推理摘要、事实/假设/未知项、计划与决策依据、工具观察、证据、Finding、任务状态和经治理的经验总结。它也不把 VectorStore 等同于 Memory Store：向量索引是可重建的派生检索设施，不是权威事实源。

## 2. 当前复用边界与成熟度

可复用基础：

- `MemoryStore` 已有 File/SQLite/InMemory 后端、tenant/agent/session 隔离、generation、digest、恢复、redaction 和 GC；
- `MemoryAssemblyInput` 已统一 System/Task/Working/Retrieval/Tool/Skill 六类 Prompt 源，并有确定性预算和引用保留；
- `MemoryCompactor` 已有 truncate/structured summary/fallback、取消和审计报告；
- VectorStore/Faiss 已有 metadata filter、持久化 generation、RAG citation；
- SessionStore、ExecutionContext、ChildTask、Effect Journal 已有 session/task/run 相关状态和 CAS/恢复基础；
- Skill Registry/Loader/Runtime 已有 L1/L2 渐进披露、权限、来源、签名和 generation pin。

当前满足度约 **25%**。关键缺口是：没有 org/principal/project/workspace/task/run/turn namespace，没有权威/可信/敏感/时效模型，没有动态 View、冲突解析、受治理晋升、项目指令层级、记忆快照和跨作用域防泄漏门禁。

## 3. 作用域、语义和状态三轴模型

### 3.1 作用域轴

| 层级 | Scope key | 典型内容 | 默认写策略 |
|---|---|---|---|
| L0 Platform/System | `platform/agent` | 安全约束、运行规则、能力边界、基础 Agent 指令 | 签名、只读、fail-closed |
| L1 Organization/Principal | `tenant/org/principal` | 组织政策、团队规范、个人偏好与用户画像 | 组织只读；个人经授权可写 |
| L2 Project/Workspace | `project/workspace/path` | 目标、技术路线、架构、资源、AGENTS、ADR、计划、技术债 | Git/versioned；评审写入 |
| L3 Task/Run | `task/run/plan/node/attempt` | intake、理解、计划、状态、证据、Finding、artifact、历史尝试 | durable event/CAS |
| L4 Turn/Working | `session/turn` | 当前提示词、验收要求、最近对话、工具观察、RAG、Skill、临时摘要 | 短期/checkpoint；仅可候选晋升 |

Organization 与 Principal 共处 L1 但必须拥有不同 namespace、ACL、retention 和 consent，禁止用一个共享 profile 混合。

### 3.2 信息语义轴

`MemoryKind` 至少覆盖：

- `Semantic`：事实、概念、项目资源、偏好；
- `Episodic`：历史动作、尝试、失败、结果和环境；
- `Procedural`：政策、AGENTS、SOP、Skill 和操作约束；
- `Evidentiary`：EvidenceRecord、引用、测试、截图、artifact 和 Finding；
- `Operational`：Task/Run/Plan/Node/Approval 当前状态；
- `Conversational`：用户提示、澄清、验收要求和对话消息。

作用域与语义正交；例如 Project scope 可以同时存在 procedural 架构规则、semantic 资源目录、episodic 迁移经验和 evidentiary 基准报告。

### 3.3 可信状态轴

```text
Raw → Candidate → Verified → Authoritative
                 ↘ Rejected
任意状态 → Superseded / Tombstoned / Expired
```

模型摘要、RAG 命中和任务成功只能生成 `Candidate`；不得自动提升为组织/系统权威记忆。所有提升、纠错、降级、撤销和遗忘必须保留 provenance、actor、policy、decision 和前后 revision。

## 4. 核心数据契约

```cpp
struct MemoryScopeRef {
    std::string tenant_id;
    std::optional<std::string> organization_id;
    std::optional<std::string> principal_id;
    std::optional<std::string> agent_id;
    std::optional<std::string> project_id;
    std::optional<std::string> workspace_id;
    std::optional<std::string> path_scope;
    std::optional<std::string> task_id;
    std::optional<std::string> run_id;
    std::optional<std::string> turn_id;
};

struct MemoryRecord {
    std::string schema_version;
    std::string memory_id;
    MemoryScopeRef scope;
    MemoryKind kind;
    MemoryAuthority authority;
    MemoryStatus status;
    std::string summary;
    ArtifactRef content;
    Provenance provenance;
    AccessPolicy access;
    RetentionPolicy retention;
    Freshness freshness;
    std::vector<std::string> supports;
    std::vector<std::string> conflicts_with;
    std::optional<std::string> supersedes;
    std::string revision;
    std::string canonical_digest;
};
```

每条记录必须区分 event time、ingest time 和 valid time；必须支持 optimistic CAS、append-only history、tombstone、legal hold、purpose limitation、redaction 和 crypto-shredding/索引删除。正文可存 Artifact/Object Store，Memory Store 保存规范化元数据和内容引用。

## 5. Provider 与存储架构

```text
MemoryProviderRegistry
├── PlatformInstructionProvider
├── OrganizationMemoryProvider
├── PrincipalMemoryProvider
├── ProjectInstructionProvider      # AGENTS/override/root→cwd
├── ProjectKnowledgeProvider         # architecture/ADR/resources/plans
├── TaskRunMemoryProvider             # Plan/Evidence/Finding/Run refs
├── TurnWorkingMemoryProvider         # prompt/history/tool observations
├── SkillProceduralMemoryProvider     # metadata first, body on demand
└── RetrievalMemoryProvider           # external/internal RAG candidates

Authoritative/Versioned Stores
        ↓ change feed
Derived Lexical/Vector/Graph Indexes
        ↓ rebuildable
MemoryViewEngine
```

Provider 不复制上游事实：PlanStore、EvidenceStore、RunStore、Skill Registry、项目 Git 文档仍各自是单一事实源，MemoryRecord 通过稳定引用、revision 和 digest 统一访问。SQLite 是本地首个实现；WP4.8 再提供 PostgreSQL/Object Store/remote index，不改变 API 语义。

## 6. 指令优先级与冲突规则

禁止简单使用“最后写入覆盖”或“相似度最高胜出”。解析顺序为：

1. 不可覆盖的 Platform 安全与数据边界；
2. tenant/organization policy；
3. agent/principal 持久偏好；
4. project root 到 workspace/cwd 的路径特异性指令；
5. 已批准 Task/Plan/AcceptanceContract；
6. 当前用户提示与澄清；
7. Skill 程序性建议；
8. RAG、网页、工具输出等不可信数据。

“特异性更高优先”只在同 authority class 和可覆盖规则内成立。用户可以改变任务意图和普通偏好，但不能覆盖安全、租户隔离、强制验收或未授权副作用边界。冲突必须生成结构化 `MemoryConflict`；无法确定时进入 WP4.3 durable clarification/approval，不得猜测。

## 7. 动态 Memory View

```cpp
struct MemoryViewSpec {
    MemoryViewMode mode;
    MemorySubject subject;
    std::vector<MemoryScopeRef> scopes;
    std::vector<MemoryKind> allowed_kinds;
    MemoryAuthority authority_floor;
    FreshnessPolicy freshness;
    IsolationProfile isolation;
    ContextBudget budget;
};

struct MemoryView {
    std::string snapshot_id;
    std::string view_digest;
    std::vector<MemoryRecordRef> selected;
    std::vector<MemoryExclusion> excluded;
    MemoryViewManifest manifest;
};
```

内置 profile：

| View | 强制内容 | 隔离/裁剪重点 |
|---|---|---|
| Intake | 用户要求、授权、组织/项目约束、相关历史任务 | 不加载无关工具历史 |
| Investigation | 项目地图、事实源、历史故障、RAG、可用 Skill | 外部内容仅作为数据 |
| Planning | 目标、证据、未知项、技术路线、资源、依赖、验收要求 | 去除无关 turn 噪声 |
| Execution | 当前 PlanNode、上游合同、局部事实、工具/Skill、已提交 effect | node 最小权限和最小上下文 |
| Verification | AcceptanceContract、独立证据、待验产物 | 隔离执行者未验证自述 |
| Replan | 原计划、失败证据、影响范围、仍有效约束 | 标记旧结论失效范围 |
| Resume | 固定 plan/memory/evidence snapshot、pending work、effects | 恢复时禁止静默换视图 |
| Handoff | 上下游合同、交付、剩余风险、证据摘要 | 不转移秘密和私有 scratch |

View pipeline 固定为：Identity/Scope resolution → mandatory policy → provider candidates → ACL/sensitivity → trust/freshness/conflict → hybrid retrieval → diversity/dedupe → mode budget → materialization → prompt projection。

每个 View manifest 记录 selected/excluded ID、revision/digest、原因、预算、裁剪、冲突、脱敏、policy revision 和 provider generation；相同 snapshot/spec 必须得到相同 view digest。

## 8. 检索、预算与渐进披露

ACL、tenant 和 scope filter 必须在相似度计算前执行。候选排序组合 semantic/lexical relevance、scope proximity、authority、freshness、verified confidence、workflow usefulness，并惩罚 conflict、staleness 和 redundancy。

System/组织强制政策和已批准 Task/AcceptanceContract 使用预留预算，溢出时 fail-closed；项目知识、历史 episode、RAG 和 Skill 采用渐进披露。AGENTS 只作为短地图和路由入口，详细设计、资源、计划和证据按需读取。Skill 启动时只加载 metadata，命中后 pin generation 再加载完整指令和 reference。

现有 `MemoryAssembly` 作为兼容适配器保留；v2 将“来源/作用域/权威/模式/预算决策”前置，最后再投影到 System/Task/Working/Retrieval/Tool/Skill Prompt 通道。

## 9. Consolidation、晋升与遗忘

- Hot path 只追加原始事件、证据、消息和任务状态；
- Task close/background consolidator 从已完成轨迹生成 candidate fact/episode/doc patch/Skill improvement；
- candidate 必须通过 provenance、schema、冲突、事实验证和 policy；
- Principal 偏好写入需要 consent；Project 权威文档通过 diff/test/review；Organization/System 通过 HITL 或外部管理面；
- supersede 保留旧 revision 和有效期，不物理覆盖历史；
- forget 请求传播到正文、索引、cache、snapshot 可见性和备份策略，并产生可审计 completion/inconclusive 结论；
- Consolidator 使用隔离模型/profile，不获得高于输入记录的 authority，也不得持久化私有 chain-of-thought。

## 10. 可执行任务

| ID | 任务 | 交付与完成条件 |
|---|---|---|
| 4.9.1 | Taxonomy 与 invariants | 五层 scope、六类 kind、状态机、非目标和威胁模型；非法组合 fixture |
| 4.9.2 | Canonical schemas | MemoryScope/Record/Ref/Conflict/Snapshot/ViewSpec/ViewManifest，version/digest/migration |
| 4.9.3 | Authority/lifecycle model | authority、trust、sensitivity、freshness、valid time、supersede/tombstone/retention |
| 4.9.4 | Platform/System provider | 签名/只读指令、agent identity、能力/安全边界、fail-closed |
| 4.9.5 | Organization/Principal providers | 独立 namespace、ACL、consent、组织只读与个人 profile 更新 |
| 4.9.6 | Project/AGENTS resolver | global + root→cwd + override/fallback、path specificity、Git revision、地图式渐进披露 |
| 4.9.7 | Task/Run provider | Intake/Understanding/Plan/Node/Approval/Evidence/Finding/Artifact/Attempt 稳定引用 |
| 4.9.8 | Turn/Working provider | 当前 prompt、验收要求、历史尝试、tool observation、短期 TTL/checkpoint |
| 4.9.9 | Skill procedural adapter | catalog/body/reference 分层、policy、generation/digest pin、按需加载 |
| 4.9.10 | Evidence/RAG adapter | 内外部记录、citation/provenance、外部数据不具指令权、derived index |
| 4.9.11 | Scope-first hybrid retrieval | ACL 前置、lexical/vector/graph/recency、dedupe/diversity、rebuild |
| 4.9.12 | Memory View profiles | Intake/Investigation/Planning/Execution/Verification/Replan/Resume/Handoff 版本化规范 |
| 4.9.13 | Dynamic View router | workflow event 驱动转换、角色隔离、provider budget/deadline/cancel |
| 4.9.14 | Precedence/conflict resolver | authority × specificity × freshness；冲突、澄清和 fail-closed |
| 4.9.15 | Assembly v2 | mandatory reserve、渐进披露、mode budget、citation、deterministic manifest |
| 4.9.16 | Snapshot/pinning/recovery | memory/provider/index/view revision 固定到 Run checkpoint，重启重放一致 |
| 4.9.17 | Consolidation/promotion | hot/background candidate、事实校验、跨层晋升、doc/Skill patch |
| 4.9.18 | Governance/forget | RBAC/HITL、审计、retention/legal hold、纠错、删除传播、crypto-shredding |
| 4.9.19 | CLI/Web Memory Inspector | scope/view/source/conflict/budget/promotion/forget UI；权限脱敏与真实截图 |
| 4.9.20 | Migration/vertical E2E/eval | v1 adapter/dual-read、跨层复杂任务、恢复/泄漏/污染/指标门禁 |

## 11. 与其他工作包的接口

- WP4.0 只消费 Investigation/Planning/Replan View，并把理解、计划和专业推理摘要写入 L3；
- WP4.1 只消费 Verification View，Finding/Evidence 可回写 L3，但不能覆盖被验产物；
- WP4.2 checkpoint 原子引用 MemorySnapshot/View/Provider generation；
- WP4.3 承载组织/项目晋升、敏感写入、纠错和 forget approval；
- WP4.4 将外部/RAG/项目可编辑内容视为不可信数据，隔离 memory connector 与 secret；
- WP4.5 关联 retrieve/select/exclude/conflict/compact/promote/view-transition span；
- WP4.6 评测 scope、检索、利用、忠实、污染、过期、压缩、成本和任务收益；
- WP4.7 验证真实 provider、重启、跨 session/project、权限轮换和 forget；
- WP4.8 迁移为 remote store/index/cache/lease，但保持 snapshot/CAS/ACL 语义。

## 12. 五层验收与指标

- 功能：五层读取、动态 View、受控写入、晋升、纠错、遗忘、恢复；
- 模块：schema/state/precedence/ACL/time/budget/property tests，恶意 scope/路径/metadata fixture；
- 集成：Planning/Run/HITL/Skill/RAG/Evidence/Assurance/Telemetry 的固定 snapshot；
- 综合：跨 session 的真实复杂任务能继承有效项目知识而不泄漏其他 org/project/task，Verifier 使用独立视图拒绝虚假完成；
- 指标：scope leak、mandatory retention、false memory、conflict/stale detection、retrieval precision/recall/nDCG、citation faithfulness、view reproducibility、compaction retention、token efficiency、task success delta、promotion precision 和 forget completion。

硬门槛：跨 tenant/org/project/task 未授权泄漏率为 0；mandatory directive retention 为 100% 或 fail-closed；未批准的 Authoritative promotion 为 0；固定 snapshot/spec 的 view digest 必须一致；恢复不得静默切换 Memory revision。

## 13. DoD 与回滚

一个 production-like 任务跨多个 turn、session 和进程重启完成调查、规划、执行、验收与重规划：系统/组织/项目规则正确生效，任务状态和证据可恢复，Skill/RAG 按需加载，Verifier 具有隔离视图，任务结束只产生受治理 candidate，批准后才晋升项目记忆；全程可由 ViewManifest、Run checkpoint、Audit/OTel 和 AcceptanceReport 重放。

先以 `MemoryProviderRegistry + MemoryViewEngine` feature flag 包装现有 AgentLoop，保留 v1 Assembly/MemoryStore 只读兼容；实施 dual-read/shadow-view，对 selected IDs、Prompt digest 和结果做差异报告。新路径未通过隔离、恢复和污染门禁前不得设为默认。回滚只切换读取路径，不删除 v2 records/index；schema 不兼容时显式拒绝或迁移，禁止降级后误读高权限记忆。

## 14. 外部设计依据

- OpenAI Codex 将全局指令、项目根到 cwd 的层级指令、Skill metadata 和环境上下文组合进模型输入：[Unrolling the Codex agent loop](https://openai.com/index/unrolling-the-codex-agent-loop/)。
- OpenAI Harness Engineering 建议把 `AGENTS.md` 作为短地图，将详细知识、执行计划和质量状态放入结构化、可版本化并可机械检查的仓库事实源：[Harness engineering](https://openai.com/index/harness-engineering/)。
- LangGraph 将线程内短期状态与自定义 namespace 的跨线程长期记忆分离，并区分 semantic、episodic、procedural memory：[Memory overview](https://docs.langchain.com/oss/python/concepts/memory)。
- Deep Agents 明确区分 user/agent/organization scope、按需 Skill、后台 consolidation 和组织共享记忆只读策略：[Deep Agents Memory](https://docs.langchain.com/oss/python/deepagents/memory)。
