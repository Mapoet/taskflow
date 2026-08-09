# Phase 4 分工作包规划索引

**状态事实**：[phase-4-status.md](../phase-4-status.md)  
**当前总体计划**：[phase-4-plan-v2.md](../phase-4-plan-v2.md)  
**v1 历史计划**：[phase-4-plan.md](../phase-4-plan.md)  
**v2 原始需求记录**：[phase-4-v2.md](../phase-4-v2.md)  
**阶段章程**：[phase-4.md](../phase-4.md)

本目录现保留 WP4.0–WP4.9 共 **136 个 v1 稳定任务 ID**；v2 计划另定义 **75 个** `P4-V2-*` 可落地计划节点，不重排旧 ID。WP4.9 虽使用尾部编号以避免重排既有任务，但属于 P0，逻辑上在 Durable/HITL 基础之后、认知规划和五层验收垂直闭环之前实施。

v2 将 Role-based LLM Workflow Runtime 设为新的横向 P0 能力，并以“LLM Cognitive Plane + Deterministic Control Plane”为统一边界。当前完成事实只以状态矩阵和 AcceptanceReport 为准。

## 工作包

| WP | 详案 | 优先级 |
|---|---|---:|
| WP4.0 | [Task Cognition & Planning](./wp4.0-task-cognition.md) | P0 |
| WP4.1 | [Evidence-driven Assurance](./wp4.1-assurance-harness.md) | P0 |
| WP4.2 | [Durable Run Kernel](./wp4.2-durable-run.md) | P0 |
| WP4.3 | [HITL & Policy](./wp4.3-hitl-policy.md) | P0 |
| WP4.4 | [Unified Sandbox](./wp4.4-sandbox-runtime.md) | P0/P1 |
| WP4.5 | [Observability & SLO](./wp4.5-observability-slo.md) | P1 |
| WP4.6 | [Evaluation Harness](./wp4.6-evaluation-harness.md) | P1 |
| WP4.7 | [Live Certification](./wp4.7-live-certification.md) | P1 |
| WP4.8 | [Distributed Control Plane](./wp4.8-distributed-control-plane.md) | P2 |
| WP4.9 | [Multi-layer Memory & Dynamic Context Views](./wp4.9-multilayer-memory.md) | P0 |

## 执行治理

- [需求—计划—证据追溯矩阵](./traceability-matrix.md)
- [规划—执行台账](./execution-ledger.md)
- [架构与验收决策日志](./decision-log.md)

## 维护约束

1. 当前完成状态只写入 `phase-4-status.md`。
2. 本目录详案定义目标和稳定任务 ID，不因一次实现覆盖历史计划。
3. 执行时先登记 ledger，再修改代码；偏离计划先记录 decision。
4. 每个任务至少更新 traceability 的 source、test、evidence 和 acceptance 列。
5. UI/视觉任务必须保留真实截图；Live 任务必须证明 executed，不能以 skip 作为通过。
6. `[x]` 只由最终 AcceptanceReport 驱动。
7. Memory View 必须记录 snapshot/spec/view digest 和 selected/excluded 原因；Prompt 文本、向量索引或摘要不能替代权威 MemoryRecord。
