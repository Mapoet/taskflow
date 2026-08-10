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
| `P4-DOC-MEMORY-20260809` | `P4-F0 / plan-v2` | accepted | 2026-08-09 | Codex / user approval | 新增 WP4.9；融合章程、状态、总体计划、WP4.0–4.8 和治理档案 | PASS：17 文档、136 个唯一 ID、0 重复、0 失效本地链接；WP4.9 SHA-256 `0defa55f8559a7f6bfb5a5066e74fabe06ccc18b02c05b2c1d7553c299e6c206` | 用户于 2026-08-09 批准落盘；内容与结构验收 PASS |
| `P4-F0R-F8-VERTICAL-20260809` | `P4-F0R–P4-F8 / plan-v3` | partial | 2026-08-09 | Codex / user continuous approval | 可移植性修复；共享契约；SQLite Run/Memory；Cognition/Assurance；Sandbox/Telemetry/Eval/Live/Queue 参考语义；跨模块纵向测试 | PASS：`phase4-offline` 11/11、`phase3-offline` 14/14；见验收报告 | 首轮离线纵向闭环 accepted；各 WP 生产 DoD、真实 Live、远程 durability/HA 保持 partial |
| `P4-F2D-DURABLE-STORES-20260809` | `P4-F2D / plan-v3.1` | partial | 2026-08-09 | Codex / user continuous approval | 新增 SQLite Evidence/Plan Store 与 ApprovalStore；跨实例 CAS、父 digest、scope isolation、pending/TOCTOU/SoD/expiry/revocation | PASS：`phase4-offline` 13/13、`phase3-offline` 14/14；见增量报告 | 已实现的 durable store 子集 accepted；AcceptanceReport store 与跨 store 原子协调 pending |
| `P4-DOC-V2-BASELINE-20260809` | `P4-V2-F0 / phase4-v2-plan-r1` | implemented | 2026-08-09 | Codex / user explicit approval | 保留 v1，新增 v2 正式计划；按扩展目标重估 status；更新 traceability、decision、ledger 和导航 | PASS：75 个唯一 v2 计划节点、41 个本地链接且 0 失效；plan SHA-256 `4d8b2db253fef9b09feb53bec30380c851fd5247a6176526724c85295043fd3d` | 文档结构与引用校验 PASS，内容等待用户验收；`R4V2-01`–`R4V2-08` 代码实施仍为 planned/partial |
| `P4-V2-F1L-OFFLINE-CORE-20260809` | `P4-V2-F1L.1–F1L.12 / phase4-v2-plan-r1` | partial | 2026-08-09 | Codex / user explicit approval | 新增 Role Runtime typed contracts、registries/router/schema gate、SQLite manifest store/recovery、calibration/independence/capability/View gates、usage/latency、Telemetry/Audit bridge、受保护 provider 参数边界、fake fallback/repair、AgentLoop optional adapter；CI minimum 13→17 | PASS：`phase4-llm-runtime` 4/4、legacy LLMClient/AgentLoop 6/6、`phase4-offline` 17/17、`phase3-offline` 14/14；详见 `acceptance-P4-V2-F1L-20260809.md` | 离线核心 accepted；全路径不可绕过、独立 StructuredOutputPolicy、overlay/default revision、GraphExecutor/timeout/live provider matrix 保持 pending，F1L 总体不标 complete |
| `P4-V2-F2C-OFFLINE-CORE-20260809` | `P4-V2-F2C.1–F2C.10 / phase4-v2-plan-r1` | partial | 2026-08-09 | Codex / user explicit approval | 新增 RoleRuntime cognition adapter、strict stage/checkpoint、SQLite CAS、Intake/Strategy/只读 Investigation/Synthesis/Boundary/Planner/独立 Critic/Revision/HITL、GraphExecutor template、kill/restart recovery；CI minimum 17→20 | PASS：`phase4-cognition-v2` 3/3、`phase4-offline` 20/20、`phase3-offline` 14/14；详见 `acceptance-P4-V2-F2C-20260809.md` | 离线核心 accepted；真实 investigator/provider/calibration、跨 Store 原子提交、ApprovalStore 直连、默认不可绕过和 live/SLO 保持 pending，F2C 总体不标 complete |
| `P4-V2-F3M-OFFLINE-CORE-20260810` | `P4-V2-F3M.1–F3M.9 / phase4-v2-plan-r1` | partial | 2026-08-10 | Codex / user explicit approval | 新增九阶段 RoleRuntime memory workflow、strict/SQLite checkpoint、scope-first/provider 二次隔离、candidate/task-state、query/rerank、dynamic View、HITL promotion/forget executor、v1 dual-read、GraphExecutor/restart；SQLite helper 公共化；CI minimum 20→24 | PASS：`phase4-memory-v2` 4/4、`phase4-offline` 24/24、`phase3-offline` 14/14；详见 `acceptance-P4-V2-F3M-20260810.md` | 离线核心 accepted；真实 calibration、生产 hybrid retrieval、跨 Store atomic/ApprovalStore、cross-scope migration、默认强制、Live/HA/Inspector 保持 pending，F3M 总体不标 complete |
| `P4-V2-F4V-OFFLINE-CORE-20260810` | `P4-V2-F4V.1–F4V.10 / phase4-v2-plan-r1` | partial | 2026-08-10 | Codex / user explicit approval | 新增 Verification Planner、Manifest deterministic oracle、Code/Architecture/Domain/Security/Completeness/Resolver RoleRuntime stages、能力交集与角色隔离、evidence closure/conflict resolution、强 oracle arbiter、SQLite checkpoint/report 原子提交、restart 与 GraphExecutor；CI minimum 24→28 | PASS：`phase4-assurance-v2` 4/4、`phase4-offline` 28/28、`phase3-offline` 14/14；详见 `acceptance-P4-V2-F4V-20260810.md` | 离线核心 accepted；真实 Sandbox/ToolBus oracle adapters、生产 calibration/benchmark、跨 Store atomic、F5R remediation/replan、默认强制、Live/UI 保持 pending，F4V 总体不标 complete |
| `P4-V2-F5R-OFFLINE-CORE-20260810` | `P4-V2-F5R.1–F5R.7 / phase4-v2-plan-r1` | partial | 2026-08-10 | Codex / user explicit approval | 新增 typed ImpactInventory/FindingBinding/ImpactGraph、Impact Analyst/Remediation Planner/Reverification Planner RoleRuntime stages、确定性下游失效闭包、授权/回滚/风险校验、PlanValidator+父摘要 CAS、PDP/HITL anti-downgrade、token/cost/deadline、digest/freshness selective reuse、SQLite recovery、GraphExecutor；CI minimum 28→32 | PASS：`phase4-remediation-v2` 4/4、`phase4-offline` 32/32、`phase3-offline` 14/14；详见 `acceptance-P4-V2-F5R-20260810.md` | 离线控制面 accepted；修复动作实际执行、新 artifact digest 回填、自动 F4V rerun、ApprovalStore 直连、跨 Store atomic、生产 calibration/live 保持 pending，F5R requirement 不标 complete |
| `P4-V2-F6E-OFFLINE-CORE-20260810` | `P4-V2-F6E.1–F6E.9 / phase4-v2-plan-r1` | partial | 2026-08-10 | Codex / user explicit approval | 新增 typed suite/run/Judge/calibration/metric/upgrade/report/checkpoint；隔离 Evaluation View；匿名候选/顺序反转/label 隐藏；Primary/Secondary/Adjudicator RoleRuntime 与独立性；确定性 agreement/kappa/bias/variance/GT accuracy/Wilson CI；24 项领域指标、flaky/critical/regression；PDP/HITL upgrade/rollback；SQLite terminal atomic report/restart；GraphExecutor；CI minimum 32→36 | PASS：`phase4-judge-v2` 4/4、`phase4-offline` 36/36、`phase3-offline` 14/14；详见 `acceptance-P4-V2-F6E-20260810.md` | 离线控制面 accepted；真实分层数据集/人工基线/生产 Judge calibration、nightly scheduler、signed report、trend/SLO、跨 Store atomic/default mandatory gate 保持 pending，F6E requirement 不标 complete |
| `P4-V2-F7L-CONTROL-PLANE-20260810` | `P4-V2-F7L.1–F7L.7 / phase4-v2-plan-r1` | partial | 2026-08-10 | Codex / user explicit approval | 新增 environment/profile/matrix/cell/checkpoint/report typed contract；RoleRuntime invocation manifest adapter；规划/执行/验证/Judge identity 与 provider/model/group independence；read-only/blind/oracle；timeout/rate/malformed/fallback/restart/cost-cap；SQLite CAS/restart；审批、signer/verifier、expiry/revision/alert；GraphExecutor；显式 no-skip production artifact gate；CI minimum 36→41 | PASS：`phase4-live-v2` 5/5、`phase4-offline` 41/41、`phase3-offline` 14/14；生产 gate 缺配置经验证返回 `BLOCKED`/exit 2；详见 `acceptance-P4-V2-F7L-20260810.md` | 离线控制面 accepted；当前未提供 production credentials/role matrix/external scheduled runner/KMS report，故没有生产 `executed=true` 证据，F7L requirement 不标 complete |
| `P4-V2-F8U-UI-PLANE-20260810` | `P4-V2-F8U.1–F8U.6 / phase4-v2-plan-r1` | partial | 2026-08-10 | Codex / user explicit approval | canonical display-safe operations snapshot；CLI/Web/TUI/ImGui 同源投影；Plan/Evidence、Memory、Invocation、五层 Assurance、Live、HITL；Web snapshot/action route；a11y/responsive；CI minimum 41→42 | PASS：targeted 6/6、`phase4-offline` 42/42、`phase3-offline` 14/14；Web/TUI/ImGui 真实运行截图和 HITL request/refresh；详见 `acceptance-P4-V2-F8U-20260810.md` | 统一离线 UI plane accepted；production Store assembler、ApprovalStore executor、真实 mobile screenshot 和 production Live snapshot pending，故 `R4V2-08` 保持 partial |

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
