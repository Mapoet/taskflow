# Phase 4 架构、规划与验收决策日志

**用途**：记录会影响范围、schema、兼容性、风险和验收标准的决策。普通实现细节无需登记。

## 决策状态

`proposed`、`approved`、`rejected`、`superseded`、`deprecated`。

## 已确认决策

### D4-001 — 认知规划与证据验收列为 P0

- **日期**：2026-08-09
- **状态**：approved
- **决策者**：用户
- **背景**：直接 ReAct 执行缺少专业任务理解、上下游规划和总体完备性验证。
- **决定**：WP4.0/WP4.1 与 Durable Run/HITL 同为 P0；Phase 4 不能只补运行基础设施。
- **影响**：Run schema 必须包含 plan/evidence/acceptance revision；分布式控制面后移。

### D4-002 — 单次 Assurance 与总体 Evaluation 分离

- **日期**：2026-08-09
- **状态**：approved
- **决定**：WP4.1 判断一个任务是否完成；WP4.6 判断系统版本在任务集上的总体表现。共享证据模型，不共享最终裁决语义。

### D4-003 — 现有 Verifier 仅作为 Assurance 适配器

- **日期**：2026-08-09
- **状态**：approved
- **依据**：现有 Verifier 无工具/外部 API，只检查 draft answer。
- **决定**：保留兼容，不授予最终 accepted 权限。

### D4-004 — 分布式控制面后置

- **日期**：2026-08-09
- **状态**：approved
- **决定**：先冻结 Run/Plan/Evidence/Acceptance/Memory/Sandbox 语义并完成单机 Live，再实施 WP4.8。

### D4-005 — 多层记忆与动态 Context View 列为 P0

- **日期**：2026-08-09
- **状态**：approved
- **决策者**：用户
- **背景**：当前 MemoryStore/Assembly/Compaction/RAG/Skill 只能提供 session 级静态 Prompt 装配，无法表达系统、组织/个人、项目、任务和本轮的权限、权威、生命周期及场景差异。
- **决定**：新增 WP4.9，采用五层 scope、六类语义、受治理生命周期和动态 Memory View；逻辑实施位置在 Durable/HITL 基础之后、认知规划与验收闭环之前。
- **兼容**：保留既有 4.0–4.8 任务 ID；使用 4.9 编号不代表低优先级；现有 v1 Assembly/Store 作为 feature-flag/dual-read 兼容路径。

### D4-006 — Memory Store 与 Prompt View 分离

- **日期**：2026-08-09
- **状态**：approved
- **决定**：权威事实、历史和状态保存在版本化 Store/上游事实源；Prompt 是按 scope、workflow phase、authority、freshness、ACL 和预算生成的可重现 Materialized View。
- **影响**：Run checkpoint pin MemorySnapshot/View digest；Vector/lexical/graph index、summary、compaction 和 rendered Prompt 都是可重建派生物，不能覆盖原始 evidence 或 authoritative record。

### D4-007 — 记忆只能受控晋升

- **日期**：2026-08-09
- **状态**：approved
- **决定**：模型摘要、RAG 命中、工具观察和单次任务成功只能生成 candidate；Principal 写入需要 consent，Project 权威写入需要 diff/验证/评审，Organization/System 写入必须 HITL 或外部管理面批准。
- **安全依据**：外部或可编辑内容默认是数据而非高权限指令；consolidator 不能授予自身更高 authority，也不能审批自己的晋升。
- **验收**：未批准 Authoritative promotion 为 0，所有晋升、纠错、撤销和 forget 均有 provenance、decision 和传播证据。

### D4-008 — Phase 4 v2 采用 LLM 认知面与确定性控制面

- **日期**：2026-08-09
- **状态**：approved
- **决策者**：用户
- **关联 requirement / plan revision**：`R4V2-00`–`R4V2-08` / `phase4-v2-plan-r1`
- **事实与证据**：当前 Planning 只有单次抽象 `CognitionModel::draft()`；Assurance 有插件 Harness 但尚无专业 LLM verifier；Phase 4 Memory View 和 Governance 主要是确定性逻辑，LLM 主要用于 legacy compaction。已有确定性骨架必须复用，但不能声称已经形成 LLM 驱动规划—记忆—验证系统。
- **备选方案**：分别在 Planning、Memory、Assurance 中直接增加零散模型调用；该方案会复制配置、路由、审计、恢复和权限逻辑，无法证明角色独立性。
- **决定与理由**：先实施横向 `P4-V2-F1L Role-based LLM Workflow Runtime`，再接入多阶段 Cognition、Memory Workflow、专业 Assurance 和 Judge。LLM 负责分析、调查、综合、规划、语义加工、批评和专业解释；Schema、ACL、Authority、Policy、CAS、Approval、强 Oracle 和最终 Arbiter 保持确定性。
- **独立性**：Planner/Executor 不得成为自己的唯一 Verifier；关键任务至少两个独立 verifier。相同模型只更换 Prompt 不自动满足 independence policy。
- **推理记录**：不持久化隐藏 chain-of-thought，只记录 claims、evidence、assumptions、unknowns、alternatives、decision rationale、risks、counterexamples 和 confidence。
- **安全、兼容、迁移和回滚影响**：新路径使用 feature flag、immutable profile/prompt/schema/calibration revision 和 shadow evaluation；legacy draft/verifier/compaction 先保留 adapter，可回滚到上一批准 revision。
- **验收标准变化**：原 Phase 4 45–50% 作为 v1 基线保留；按 v2 扩展目标重新计量为 35–40%。任何生产 LLM 调用必须记录实际路由、Prompt、Memory View、能力、token/cost、fallback 和 calibration manifest。

### D4-009 — F1L 采用独立 Invocation Store，并暂将 Structured Output Policy 固结于 Prompt revision

- **日期 / 状态 / 决策者**：2026-08-09 / approved-for-offline-core / Codex（依据用户批准实施 F1L）
- **关联 requirement / plan revision**：`R4V2-01` / `phase4-v2-plan-r1`
- **事实与证据**：现有 Run/Event 契约已发布且服务于跨模块状态机；LLM 调用具有高频 attempt、provider usage、fallback 和不确定恢复语义。直接扩展 Run v1 会扩大迁移和回归面。首版 Prompt revision 已原子包含 input/output schema、repair budget、redaction 和 compatibility class。
- **备选方案**：立即修改 Run schema 并把每次 LLM attempt 写成 RunEvent；或在首版同时建立独立 `StructuredOutputPolicy` registry。前者引入跨 Store 原子协调，后者在尚无跨 Prompt 复用证据时增加一次可失配 pin。
- **决定与理由**：使用 tenant-scoped SQLite `llm_invocations` 保存不可变 profile/prompt/calibration 与 CAS manifest，通过 trace/task/run/plan identity、manifest digest 和 Telemetry/Audit 与 Run 关联；首个离线核心把 Structured Output Policy 固结在 immutable Prompt revision 内，保持 schema 与修复预算原子一致。
- **安全、兼容、迁移和回滚影响**：SQLite schema version=1、WAL/FULL synchronous、私有文件权限；旧 AgentLoop 路径仍可用，显式注入 RoleRuntime 才启用新路径。回滚通过重新 pin 已批准 profile/prompt revision，不修改历史文档。
- **验收标准变化及批准**：该决策不删除 F1L 的七契约目标；独立 `StructuredOutputPolicy`、project/org overlay 和所有工作流强制接入继续列为 residual gap，因此 `R4V2-01` 只能为 partial。

### D4-010 — F2C 使用 LLM Cognitive Plane 与确定性 Planning Control Plane 分层

- **日期 / 状态 / 决策者**：2026-08-09 / approved-for-offline-core / Codex（依据用户批准实施 F2C）
- **关联 requirement / plan revision**：`R4V2-02` / `phase4-v2-plan-r1`
- **事实与证据**：LLM 适合分析 goal、gap、claim、boundary、plan 和 counterexample，但不能可靠执行 tenant isolation、能力授权、预算、evidence 引用完整性、DAG、CAS、HITL 和恢复语义。现有 Evidence/Plan/RoleRuntime 可以作为确定性边界复用。
- **备选方案**：由一个 ReAct agent 自由完成调查和计划，或把所有 stage 输出直接视为权威计划。前者无法稳定恢复和审计，后者允许模型伪造 evidence、扩大权限或自我验收。
- **决定与理由**：Intake、Strategy、Synthesis、Boundary、Planner、Critic、Reviser 是独立 RoleRuntime 语义 stage；investigator dispatch、只读/capability/budget、claim-evidence 引用、PlanValidator、Critic independence、revision CAS、HITL validator 和 terminal state 由确定性 pipeline 执行。stage artifact 和 attempt 写入 tenant-scoped Cognition checkpoint；不保存私有 chain-of-thought。
- **安全、兼容、迁移和回滚影响**：旧 `CognitionWorkflow::draft()` 保留；新 GraphExecutor template 需要显式注册。SQLite schema v1 使用 WAL/FULL/private permission。跨 Store 非原子窗口通过 stage attempt 和幂等只读调查恢复，但不宣称 provider exactly-once。
- **验收标准变化及批准**：未降低 F2C exit gate。离线核心通过仍不足以关闭真实 profiles/prompts/investigator/provider/calibration、ApprovalStore 直连、跨 Store 原子提交、默认强制接入和 live/SLO，因此 `R4V2-02` 保持 partial。

### D4-011 — F3M 使用 LLM Memory Semantic Plane 与确定性 Memory Governance Plane 分层

- **日期 / 状态 / 决策者**：2026-08-10 / approved-for-offline-core / Codex（依据用户批准实施 F3M）
- **关联 requirement / plan revision**：`R4V2-03` / `phase4-v2-plan-r1`
- **事实与证据**：现有 Memory v2 已有 scope/store/provider/view/governance，但只有静态选择和确定性写入；legacy LLM compaction 不能表达 provenance、entity/claim、conflict、task state、query/rerank 或 promotion/forget recommendation。LLM 适合语义加工，但不能可靠实施 tenant/ACL、authority、CAS、retention、legal hold、HITL 和恢复。
- **备选方案**：让一个自由 ReAct memory agent直接检索并写入权威 Store；或让各调用点零散执行 extraction/summary。前者允许 prompt injection、跨 scope 泄漏和自我晋升，后者无法统一版本、恢复、审计和质量校准。
- **决定与理由**：Extraction、Normalization、Consolidation、Conflict、Task State、Query Planning、Reranking、Dynamic View 和 Governance Recommendation 是独立 RoleRuntime stage；scope-first/provider 二次过滤、provenance/evidence 闭包、candidate-only write、record-ID closure、View Router/budget、CAS 和 promotion/forget executor 保持确定性。LLM 永远只产生 Candidate/Recommendation，不获得 authority mutation 权限。
- **安全、兼容、迁移和回滚影响**：base/dynamic View 作为 typed contract 固定进 SQLite checkpoint；旧 v1 conversation/summary 仅通过 fixed-scope dual-read provider 投影为 Observed/Candidate，不回写或获得 instruction authority。GraphExecutor template 显式注册，可回滚到原静态 Memory View。跨 scope promotion 在具备批准的 copy/migration 语义前 fail closed。
- **验收标准变化及批准**：未降低 F3M exit gate。离线核心必须证明未批准权威晋升和跨租户泄漏为 0；真实 profiles/provider calibration、可执行 hybrid index、跨 Store 原子提交、ApprovalStore 直连、Live/HA/Inspector 仍 pending，故 `R4V2-03` 保持 partial。

### D4-012 — F4V 使用 LLM Professional Judgment Plane 与确定性 Oracle/Arbiter Plane 分层

- **日期 / 状态 / 决策者**：2026-08-10 / approved-for-offline-core / Codex（依据用户批准实施 F4V）
- **关联 requirement / plan revision**：`R4V2-04` / `phase4-v2-plan-r1`
- **事实与证据**：legacy Assurance Harness 只有只读 verifier registry、EvidenceLedger 和确定性 Arbiter，未形成 Verification Planner、专业角色、角色独立、跨 verifier 解析及 durable report。LLM 适合解释代码、架构、领域、安全与完备性，但不能可靠判定构建/测试/指标/外部副作用事实，也不能审批自己的执行结果。
- **备选方案**：让一个通用 Judge 直接返回 PASS/FAIL；或让各 verifier 自行执行工具并把输出视为最终事实。前者无法证明专业覆盖与独立性，后者混淆事实获取、语义判断和最终裁决，允许自证及多数投票覆盖强反例。
- **决定与理由**：Verification Planner 与 Code/Architecture/Domain/Security/Completeness/Resolver 是独立 RoleRuntime stage；deterministic/real-system observation 通过 digest-bound Manifest oracle 进入 EvidenceLedger；tenant/task、只读 capability intersection、criterion/evidence closure、planner/executor/verifier independence、oracle strength、equal-strength conflict 和最终 Arbiter 保持确定性。LLM finding 为 calibrated/advisory evidence，不能覆盖更强反例或降低 AcceptanceContract。
- **安全、兼容、迁移和回滚影响**：legacy `AssuranceHarness` 与 `VerifierRegistry` 保留；新 workflow/GraphExecutor template 需要显式注册。Assurance SQLite schema v1 使用 WAL/FULL/private permission，并在一个事务内提交 terminal checkpoint 与 AcceptanceReport；RoleRuntime InvocationManifest 仍在独立 Store，通过 invocation/digest 绑定。真实命令或外部系统采集必须后续经 Sandbox/ToolBus adapter，当前 Manifest adapter 不执行副作用。
- **验收标准变化及批准**：未降低 F4V exit gate。强 oracle FAIL→Accepted 必须为 0；测试通过但 mandatory artifact 缺失、规划者/执行者自验收、未知 evidence、未授权能力和未解决同强度冲突均不得 Accepted。真实专业 adapter、生产 calibration/benchmark、跨 Store 原子绑定、F5R remediation、默认强制与 Live/UI 仍 pending，故 `R4V2-04` 保持 partial。

### D4-013 — F5R 使用 LLM 修复认知面与确定性影响/策略/提交面分层

- **日期 / 状态 / 决策者**：2026-08-10 / approved-for-offline-core / Codex（依据用户批准实施 F5R）
- **关联 requirement / plan revision**：`R4V2-05` / `phase4-v2-plan-r1`
- **事实与证据**：F4V 已能输出 durable Finding/AcceptanceReport，但模型 finding 无权定位真实产物依赖、修改 PlanStore、降低 mandatory criterion 或判断旧证据仍有效。PlanStore 已有父摘要 CAS，PDP/Approval 已有策略和 decision 绑定，RoleRuntime/Memory Replan View 可复用。
- **备选方案**：由同一个 verifier 直接改代码并宣布修复；或无影响分析地全量重跑。前者混淆判断、执行和自验收，后者成本不可控且不能证明证据污染边界。
- **决定与理由**：Impact Analyst、Remediation Planner、Reverification Planner 通过 RoleRuntime 产生结构化候选；可信 `ImpactInventory`、Finding→requirement/criterion/node/artifact/evidence 映射、下游闭包、能力/回滚/风险、PlanValidator、token/cost/deadline、criterion anti-downgrade、digest/freshness reuse、PlanStore CAS 和 restart reconciliation 保持确定性。模型建议可以扩大到 inventory 内已知对象，但不能缩小强制影响闭包。
- **安全、兼容、迁移和回滚影响**：新增 remediation typed contracts 和 tenant-scoped SQLite schema v1，继续复用 `internal/sqlite_utils`；旧 Plan/F4V 契约不变。Plan commit 后进程死亡通过“当前 plan digest 等于 proposed digest”幂等恢复；另一 writer 改变 plan 时进入 manual review。任何 criterion 字段变化或 governed action 必须绑定 approval request digest/decision；无决定时停在 `AwaitingApproval`。
- **验收标准变化及批准**：未降低原 AcceptanceContract。污染或 stale evidence 不得复用，受影响 strong oracle 必须列入 forced rerun。当前交付只证明离线 remediation/replan/selective-reverification 控制面；不声称已经执行修复动作、产生新 artifact 或完成 F4V 二次裁决，故 `R4V2-05` 保持 partial。

### D4-014 — F6E 使用匿名多 Judge 认知面与确定性统计/发布裁决面分层

- **日期 / 状态 / 决策者**：2026-08-10 / approved-for-offline-core / Codex（依据用户批准实施 F6E）
- **关联 requirement / plan revision**：`R4V2-06` / `phase4-v2-plan-r1`
- **决策**：Primary、Secondary 和争议 Adjudicator 只接收匿名题面、环境、criterion 与 `artifact-A/B`，不得看到 ground truth、revision、profile、prompt、provider 或 model 身份；不同 Judge 必须满足 provider/model/independence-group 隔离。模型只产生 advisory verdict，label 映射、agreement/kappa/bias/variance/CI、领域指标、regression/critical gate、PDP/HITL upgrade/rollback 由确定性控制面完成。
- **替代方案及拒绝理由**：拒绝单 Judge 自评和把模型 winner 直接作为发布决定，因为存在位置偏差、共享故障、标签泄漏和弱语义证据覆盖强 oracle 的风险；拒绝把 task/executor memory 注入评测上下文，改用只含 System/Organization Instruction/Procedural/Evidentiary 的 Evaluation View。
- **验收标准变化及批准**：未把离线 fake Judge 准确率等同生产校准。真实分层数据集、人工 inter-rater 基线、生产 profile/prompt/model 校准、nightly/signed report、trend/SLO 和 live matrix 仍 pending，故 `R4V2-06` 保持 partial。

### D4-015 — F7L 将 Live 执行面、认证控制面和发布门禁分离

- **日期 / 状态 / 决策者**：2026-08-10 / approved-for-offline-control-plane / Codex（依据用户批准实施 F7L）
- **关联 requirement / plan revision**：`R4V2-07` / `phase4-v2-plan-r1`
- **事实与证据**：本地环境能实现并测试 RoleRuntime 调用证明、矩阵校验、恢复、签名和 no-skip gate，但没有获批生产 role matrix、真实 provider credentials、部署侧 scheduler/KMS 或 IdP/MCP/A2A/Sandbox endpoint；因此不能制造 `executed=true` 外部证据。
- **决定与理由**：以 `LiveCellExecutor` 隔离真实执行适配器，以确定性 validator 校验环境/Profile/Prompt/Provider/Model/Region/Calibration、依赖、只读/blind/oracle、独立性、预算和 recovery；以 CAS Store 持久化；以可插拔 signer/verifier 和显式启用的 `phase4_live_required` 验证发布报告。默认离线套件不注册生产 gate；一旦显式启用，缺任何配置或报告必须非零失败，禁止 skip-as-pass。
- **安全、兼容、迁移和回滚影响**：旧 `live/certification.hpp` 保留兼容；新 schema 使用独立 SQLite 表和 secret reference，报告不保存 secret value。生产 gate 的 HMAC key 仅从运行环境读取并恒定时间比较；生产部署可以通过相同 signer/verifier 接口替换为 KMS asymmetric signing。
- **验收标准变化及批准**：未降低 F7L exit gate。mock RoleRuntime 5/5 与 gate 的 `BLOCKED` 测试只接受为控制面证据；在真实 production chain 完整执行、签名、审批且未过期前，`R4V2-07` 必须保持 partial。

### D4-016 — F8U 使用 canonical display projection 隔离控制面事实与 UI 适配器

- **日期 / 状态 / 决策者**：2026-08-10 / approved-for-offline-ui-plane / Codex（依据用户批准实施 F8U）
- **关联 requirement / plan revision**：`R4V2-08` / `phase4-v2-plan-r1`
- **事实与证据**：现有 CLI/Web/TUI/ImGui 各自消费流式 answer/tool event，没有可表达 Plan/Memory/Invocation/Assurance/Live/HITL 的共享事实源；若每端直接读多个 Store，会复制 revision join、status inference、redaction 和权限逻辑，并产生跨端不一致。
- **决定与理由**：增加 `phase4.operations.v1` canonical display-safe projection；业务控制面构造 snapshot，`UIManager` 在边界 round-trip validation 后把同一 JSON 分发给所有 adapter。Schema 只含 displayable summary、identifier/version、状态和指标，不含 raw Prompt、credential、tool payload、memory content 或 private chain-of-thought。UI 不推断 Arbiter/Approval 状态。
- **安全、兼容、迁移和回滚影响**：旧 answer/tool UI 契约保持兼容；未知输入字段在 typed re-projection 时丢弃。deterministic demo HITL controller 只在 `--demo-state` 可用；普通运行缺 accountable executor 返回 409，不能伪装为 ApprovalStore decision。生产 Store assembler 和 Approval executor 后续通过相同 schema 接入。
- **验收标准变化及批准**：真实截图证明实际渲染，不证明 production data executed。当前同源 UI、桌面截图和 interaction contract accepted；production Store revision aggregation、reviewer/PDP/SoD/expiry/resume、移动真机和 production Live snapshot 未关闭，因此 `R4V2-08` 保持 partial。

### D4-017 — Residual Closure 采用顶层 durable saga 收敛已有 workflow

- **日期 / 状态 / 决策者**：2026-08-11 / approved-executing / 用户明确批准 R0→R7 连续实施
- **关联 requirement / plan revision**：`R4V2-RC-00`–`R4V2-RC-07` / `phase4-v2-residual-r1`
- **事实与证据**：F1L–F8U 单模块离线门禁通过，但 `phase4_vertical` 手工构造计划和 evidence，没有调用 Cognition、Approval、Executor、Remediation、二次 Assurance、Judge 或 Store-backed UI；各 workflow checkpoint 分散，缺少顶层恢复与 completion gate。
- **备选方案**：继续增加模块级 adapter 测试；或把全部 Store 合并成一个巨大事务。前者无法证明集成，后者把外部 effect 错误建模为 ACID exactly-once，并破坏现有边界。
- **决定与理由**：增加 `Phase4HarnessRuntime` 与 durable saga/outbox，以 pinned revision、idempotency、fencing、reconciliation 协调现有 workflow；数据库内状态可事务提交，外部 effect 通过 journal/outbox 恢复，不承诺不可能的 exactly-once。生产完成路径必须经过确定性 completion gate。
- **安全、兼容、迁移和回滚影响**：现有模块 API 和离线测试保留；新 Harness 先作为显式入口并 shadow 验证，未达到 R5E/R6L 门禁前不删除 legacy path。未知 effect、stale writer、缺审批或缺强 oracle 均 fail closed。
- **验收标准变化及批准**：没有降低 Phase 4 全局 DoD。R6L 缺外部依赖保持 blocked；R7D 可继续实现本地/loopback，但不能借此宣告 production Live 或 Phase 4 完成。

## 新决策模板

### D4-NNN — 标题

- **日期 / 状态 / 决策者**：
- **关联 requirement / plan revision**：
- **事实与证据**：
- **备选方案**：
- **决定与理由**：
- **安全、兼容、迁移和回滚影响**：
- **验收标准变化及批准**：
- **替代/废弃的 decision**：

## 必须登记的情形

- 公共 schema 或持久化格式变化；
- 计划范围实质扩大或删除交付物；
- mandatory criterion/threshold 改变；
- 接受高风险 residual risk；
- provider、sandbox、queue、store 的架构替换；
- Memory authority/precedence、namespace、write/promotion/retention/forget policy 变化；
- feature flag 切默认和旧路径退役；
- 违反原依赖顺序的紧急实施。
