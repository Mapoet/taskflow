# Phase 4 规划—执行—检验台账

**用途**：记录实际执行，不把计划文档当作完成证据。每个执行批次追加一条，不覆盖历史。

## 1. 状态流

```text
planned → approved → executing → implemented → verifying
→ accepted | partial | rejected | blocked
executing/verifying → replanning → approved
```

## 2. 批次台账

| Run | Plan task/revision | Status | Started/ended | Executor/reviewer | Changes | Evidence | Acceptance |
|---|---|---|---|---|---|---|---|
| `P4-DOC-BASELINE-20260809` | `P4-F0 / plan-v1` | implemented | 2026-08-09 | Codex / user approval | Phase 4 规划档案 | 本目录文档与链接检查 | 待文档验收 |
| `P4-DOC-MEMORY-20260809` | `P4-F0 / plan-v2` | verified | 2026-08-09 | Codex / user approval | 新增 WP4.9；融合章程、状态、总体计划、WP4.0–4.8 和治理档案 | PASS：17 文档、136 个连续且唯一 ID、围栏、本地链接、索引/状态/追踪覆盖 | 待用户内容验收 |

## 3. 执行记录模板

### RUN-ID — 标题

- **Plan task/revision**：
- **Goal / in scope / out of scope**：
- **Granted authority / approval**：
- **Upstream inputs**：
- **Memory scope / snapshot / view revision**：
- **Expected outputs**：
- **Started / ended / executor**：
- **Files and schema revisions**：
- **Commands and external calls**：
- **Side effects / effect IDs**：
- **Tests**：功能 / 模块 / 集成 / 综合 / 指标；
- **Artifacts and evidence IDs**：
- **Memory candidates / promotions / corrections / forget decisions**：
- **Deviations and new facts**：
- **Replan revision / decision IDs**：
- **Findings / residual risks**：
- **AcceptanceReport / final decision**：

## 4. 强制规则

1. 开始有副作用的执行前必须登记 plan task、revision 和授权。
2. 新事实改变范围、架构或验收阈值时，先进入 `replanning`。
3. 测试命令必须记录实际退出状态；skip、未运行和 unavailable 分开。
4. UI 证据记录真实环境、截图和关键状态；Live 记录 EnvironmentManifest。
5. `implemented` 不自动升级为 `accepted`。
6. 删除或不可逆操作记录目标、恢复性和用户批准。
7. 失败记录保留，后续修复通过新增批次关联，不改写历史结果。
8. 每次 Memory View 转换记录 snapshot/spec/view digest；权威写入、跨层晋升、纠错和遗忘记录 candidate、验证、approval 与传播结果。
