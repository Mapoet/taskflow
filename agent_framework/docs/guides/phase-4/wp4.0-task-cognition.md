# WP4.0：Task Cognition & Professional Planning Workflow

**优先级**：P0  
**依赖**：WP4.2 最小 RunStore、WP4.3 clarification/plan approval、WP4.9 Investigation/Planning/Replan View  
**下游**：WP4.1、WP4.6

## 1. 目标与非目标

目标是把原始任务转成证据支撑、上下游清楚、可执行可验收的版本化计划。不是让模型输出更长的“思考”，也不持久化私有 chain-of-thought；只保存证据、假设、备选方案、决策依据和专业推理摘要。

## 2. 当前复用边界

复用 UserInputPreprocessor、ExecutionContext、ToolBus、ChildTask/Subflow、A2A 和 Audit；MemoryStore/RAG/Skill/项目文档必须通过 WP4.9 Provider 和 Investigation/Planning View 消费。不得直接把 `ProcessedUserInput` 扩成巨型结构，也不得绕过 scope/authority/freshness 把原始检索结果塞进规划 Prompt；新增独立 cognition domain，GraphExecutor 仅通过接口消费最终 plan。

## 3. 目标模块

```text
include/agent/planning/{types,evidence_store,cognition_workflow,plan_store,plan_validator}.hpp
src/planning/{types,evidence_store,cognition_workflow,plan_store,plan_validator}.cpp
tests/phase4/planning/
```

核心类型：`TaskIntake`、`EvidenceRecord`、`EvidenceBundle`、`TaskUnderstanding`、`ChangeMode`、`PlanNode`、`ExecutionPlan`、`PlanRevisionDiff`。

## 4. 可执行任务

| ID | 任务 | 交付与完成条件 |
|---|---|---|
| 4.0.1 | Schema 与 canonical JSON | 所有核心类型、version、digest、unknown-field 和 redaction；round-trip/negative fixture |
| 4.0.2 | EvidenceStore | append-only evidence、claim relation、freshness、dedupe、tenant/task scope |
| 4.0.3 | Intake normalizer | 目标、产物、约束、授权、成功信号、事实缺口；不确定项不得猜测 |
| 4.0.4 | Reconnaissance registry | internal/external investigator 接口、工具权限、预算、取消、deadline |
| 4.0.5 | Internal investigator | repo/search/AST/build/test/git/config/runtime 证据适配器 |
| 4.0.6 | External investigator | 官方文档/标准/论文适配器，记录 URL/version/date/claim/freshness |
| 4.0.7 | Evidence synthesis | claim graph、支持/冲突、置信等级、事实/假设/未知项分离 |
| 4.0.8 | Change-mode classifier | repair/incremental/refactor/upgrade/transformation，输出 blast radius 与依据 |
| 4.0.9 | Boundary analyzer | 模块、数据/控制流、调用方、下游消费者、迁移和权限边界 |
| 4.0.10 | Plan decomposer | 无环 DAG；每节点 input/output/dependency/effect/rollback/acceptance |
| 4.0.11 | Plan validator/critic | 循环、孤儿节点、不可验收描述、越权、遗漏消费者、不可回滚检查 |
| 4.0.12 | PlanStore/revision | CAS revision、diff、父 revision、evidence digest、不可静默覆盖 |
| 4.0.13 | Workflow/gate | cognition graph、低风险自动 gate、高风险/事实不清 durable HITL |
| 4.0.14 | GraphExecutor integration | 新 planning template；计划批准前不得进入副作用执行 |
| 4.0.15 | CLI/Web presentation | 目标、证据、未知项、DAG、风险、revision；真实截图验证 |
| 4.0.16 | Vertical E2E | 真实复杂任务从 intake 到 approved plan，并注入新证据触发 replan |

## 5. 状态机

```text
Received → Normalized → Investigating → Synthesizing → Drafting
→ Critiquing → AwaitingApproval → Approved → Executing
→ Replanning → Approved
任一阶段 → Blocked/Cancelled/Failed
```

每次进入 `Investigating/Drafting/Replanning/Approved` 都写 Run checkpoint。`Approved` 必须绑定 plan digest、AcceptanceContract digest、MemorySnapshot ID 和 Planning View digest；新证据导致 View 变化时必须判断是否产生新 plan revision。

## 6. 五层验收

- 功能：模糊任务先调查；事实不足时澄清；输出可执行 DAG。
- 模块：schema、claim graph、DAG、CAS、预算、redaction property tests。
- 集成：真实 ToolBus/WP4.9 View/RAG/Skill/ChildTask/HITL/RunStore，重启后以固定 snapshot 继续规划。
- 综合：修改公共 API 的任务能找到真实消费者、迁移和验证路径。
- 指标：事实引用率、遗漏依赖率、不可执行节点率、越权率、replan precision、成本/时延。

## 7. DoD

一个 production-like 任务通过 Investigation/Planning/Replan View 完成内部调查、必要外部知识、模式判断、边界分析、DAG、critic、审批和重规划；每个节点可调度、有 AcceptanceContract，所有关键 claim 可回链到 EvidenceRecord，且规划所见系统/组织/项目/任务记忆可由 ViewManifest 重现。

## 8. 回滚

以 feature flag 启用 planning template；旧 ReAct 入口保留但标记 `unplanned_mode`。Plan schema 不兼容时明确拒绝或只读迁移，绝不把无效旧计划自动批准。
