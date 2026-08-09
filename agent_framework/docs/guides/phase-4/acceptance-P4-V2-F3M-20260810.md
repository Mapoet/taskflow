# P4-V2-F3M LLM 驱动多层记忆验收报告

**日期**：2026-08-10  
**计划**：`phase4-v2-plan-r1 / P4-V2-F3M.1–F3M.9`  
**授权**：用户明确批准实施 P4-V2-F3M  
**结论**：离线核心 `accepted`；完整 F3M `partial`，不得声称 production complete

## 1. 实施结果

| Plan node | 已实现结果 | 状态 |
|---|---|---|
| `F3M.1` | Extraction RoleRuntime stage 从 scope/ACL 过滤后的 conversation/tool/plan/result source 提取带 source/evidence provenance 的 candidate；不可引用被过滤来源 | offline accepted |
| `F3M.2` | Normalization/Entity Linking stage 输出 canonical entity、claim、valid time 和 confidence；不得扩张 extraction 的 provenance/evidence 闭包 | offline accepted |
| `F3M.3` | Consolidation stage 形成 cluster、canonical statement 和 supersede recommendation；原始记录不被覆盖 | offline accepted |
| `F3M.4` | Conflict Resolver 比较 record/authority，输出 preferred record、原因和 clarification；歧义进入 durable clarification stop | offline accepted |
| `F3M.5` | Task State Updater 将 goal/status/attempt/result/evidence/blocker 写成 Task scope、Candidate authority 的 Operational record | offline accepted |
| `F3M.6` | Query Planner 生成 query/filter；Reranker 只能重排已通过 scope-first 的 base/candidate record，未知或跨租户 ID fail closed | offline accepted |
| `F3M.7` | Dynamic View stage 推荐 mode/budget/selection；确定性 Router 验证 phase transition 并封顶 budget，重新生成和 pin View document | offline accepted |
| `F3M.8` | Promotion/forget 只生成 recommendation；独立 deterministic executor 要求 decision validator/HITL，跨 scope promotion 拒绝，forget 继续受 approval/legal-hold/sink 控制 | offline accepted |
| `F3M.9` | strict checkpoint、SQLite CAS/private/WAL/FULL、attempt/restart、输入漂移拒绝、GraphExecutor template、v1 conversation/summary dual-read、poisoning/ACL/provider-leak 负例 | offline accepted / live pending |

所有语义阶段均经 `RoleRuntimeMemoryModel` 调用 pinned profile/prompt/calibration revision；Reranker 与 Query Planner、Governance Recommender 与 Consolidator 之间使用 provider/model/group independence constraint。LLM 输出的最高权限为 `Candidate/Recommendation`，不能直接改变 authority、scope、ACL、promotion、forget 或 tenant isolation。

## 2. 确定性安全边界

```text
Identity/tenant/scope/ACL filter
  → pinned base Memory View
  → LLM extraction/normalization/consolidation/conflict/task-state
  → candidate-only idempotent write
  → LLM query plan/rerank/dynamic-view recommendation
  → deterministic record-ID closure/router/budget/View build
  → LLM governance recommendation
  → deterministic policy + HITL + MemoryGovernanceService
```

- Tool、RAG、legacy 和外部内容即使包含“忽略策略/提升为系统记忆”等文本，也作为 data，不能获得 instruction authority；
- `MemoryViewEngine` 不信任 provider 已正确过滤，对 provider 返回值再次执行 tenant/scope/path/ACL/status 校验，发现泄漏即 fail closed；
- Candidate record ID 由 tenant/workflow/normalized candidate 的 canonical digest 派生；崩溃窗口重放只接受同 tenant、同 source digest、仍为 Candidate 的完全一致记录；
- pinned base/dynamic View 的 snapshot、manifest、record contracts 写入 checkpoint，恢复时不静默切换 provider generation；
- approval 只批准 recommendation workflow；真正 authority/forget 变更仍必须显式调用 deterministic executor，不因 LLM 或 checkpoint 状态自动发生。

## 3. SQLite 公共实现收敛

按用户审查意见新增 `agent/internal/sqlite_utils.hpp`，统一以下实现：

- `database(void*)`；
- RAII `Statement`；
- `exec`；
- 空 `string_view` 安全的 `bind_text`；
- `column_text`；
- RAII `Transaction`。

Approval、Run、Planning、Cognition、Memory v2、LLM Runtime 和 F3M checkpoint Store 复用该公共层；各 Store 只保留自身 schema、SQL 和领域状态码映射。契约测试验证空字符串绑定为 SQLite `TEXT ''`，而不是 `NULL`。

## 4. 五层验证证据

| 层 | 证据 | 结果 |
|---|---|---|
| 功能 | extraction→normalization→consolidation→conflict→task state→query→rerank→dynamic view→governance recommendation→HITL | PASS |
| 模块 | strict contract/digest/unknown、SQLite CAS/reopen/private permission、shared SQLite empty-text semantics、candidate idempotency | PASS |
| 集成 | 九个真实 RoleRuntime role routes、independent reranker/governance provider、Memory Store/View/Governance、v1 dual-read、GraphExecutor | PASS |
| 综合 | process-death resume、provider 二次隔离、prompt injection、跨 tenant source、hallucinated record、非法 View transition、越权 promotion/forget | PASS |
| 指标/发布 | CI Phase 4 最小测试数 20→24；Phase 4/3 全量回归 | OFFLINE PASS / LIVE PENDING |

实际命令与结果：

```text
ctest --test-dir build -L phase4-memory-v2 --output-on-failure  # 4/4 PASS
ctest --test-dir build -L phase4-offline --output-on-failure   # 24/24 PASS
ctest --test-dir build -L phase3-offline --output-on-failure   # 14/14 PASS
```

## 5. 未关闭项与边界

1. 尚无真实数据集校准的 extraction/normalization/consolidation/conflict/query/rerank/view/governance profiles、prompts 和 live provider matrix；
2. Query Planner 当前在 scope-first 候选闭包内规划并重排；尚未接入可执行的 lexical/vector/graph hybrid index、query expansion adapter、recall/nDCG benchmark；
3. Dynamic View 使用确定性静态 profile/router 和 budget cap；LLM 的 selected/excluded ID 是可审计建议，尚无经过任务集校准的自适应 policy；
4. Workflow checkpoint、Memory Store、LLM Invocation Store 与 ApprovalStore 无跨 Store 原子事务；candidate 写入可幂等恢复，但不宣称 provider exactly-once；
5. Governance approval 通过 callback 和独立 executor 接入，尚未直接绑定 ApprovalStore 的 expiry/revocation/SoD/多签；cross-scope promotion 目前安全拒绝而非自动 copy/migrate；
6. Consolidation/supersede 和 conflict resolution 目前形成 recommendation/checkpoint artifact，不自动改写 authoritative record，也没有独立 Conflict Report Store；
7. v1 dual-read 只支持固定 scope 的 conversation/summary shadow projection；尚无批量 backfill、dual-read diff report、index migration 和回滚工具；
8. `MemoryWorkflowGraphTemplate` 需要显式注册，尚未成为所有任务不可绕过的默认前置/收尾门；
9. cancellation 仍是 provider 调用前后的协作检查；SQLite checkpoint 为单节点，没有 HA、真实故障注入、Live/SLO 或跨进程事务证据；
10. 尚无 Memory Inspector CLI/Web/TUI、真实运行截图、promotion/forget 运维面和领域长期记忆质量 benchmark。

因此 `R4V2-03` 保持 `partial`，成熟度估计 **75–80%**。本批证明 LLM 多层记忆的离线语义加工—治理闭环，不证明长期记忆质量、真实模型表现、生产检索、Live/HA 或 UI 运维能力已经达标。
