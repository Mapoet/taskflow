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
