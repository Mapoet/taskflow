# P4-V2-F2C 多阶段任务认知与规划验收报告

**日期**：2026-08-09  
**计划**：`phase4-v2-plan-r1 / P4-V2-F2C.1–F2C.10`  
**授权**：用户明确批准实施 P4-V2-F2C  
**结论**：离线核心 `accepted`；完整 F2C `partial`，不得声称 production complete

## 1. 实施结果

| Plan node | 已实现结果 | 状态 |
|---|---|---|
| `F2C.1` | `RoleRuntimeCognitionModel` 将所有语义 stage 绑定到 pinned profile/revision，传递 Memory View、capability、region、independence 和 invocation identity | offline accepted |
| `F2C.2` | Intake Analyst 严格输出 goal、constraints、acceptance、ambiguity、risk 和 clarification decision；澄清为 durable stop | offline accepted |
| `F2C.3` | Strategy stage 形成 fact gap、来源优先级、多轮只读调查步骤、预算和停止条件 | offline accepted |
| `F2C.4` | 确定性 investigator loop 执行 round/question、只读和 capability gate、required/optional failure、预算、digest 去重和逐步 checkpoint | offline accepted |
| `F2C.5` | Synthesis 形成 claim/evidence/assumption/conflict/unknown 结构；不存在的 evidence ref 和无依据 claim fail closed | offline accepted |
| `F2C.6` | Boundary stage 形成 change mode、上下游合同、in/out scope、blast radius 和风险 | offline accepted |
| `F2C.7` | Planner 形成绑定 Evidence/Understanding/Memory View 的可执行 DAG；每个节点必须有 output、acceptance、rollback/approval 和 evidence/assumption basis | offline accepted |
| `F2C.8` | Critic 使用 provider/model/group independence constraint，检查 finding、counterexample、remediation 和 evidence ref | offline accepted |
| `F2C.9` | Critic finding、用户修订请求和经外部 validator 验证的 HITL decision 形成不可变 Plan revision/CAS | offline accepted |
| `F2C.10` | SQLite stage checkpoint、attempt ledger、进程中断恢复、输入漂移拒绝、bounded loops、事件契约和 `CognitionGraphTemplate` | offline accepted |

旧 `CognitionWorkflow/CognitionModel::draft()` 保持兼容；v2 路径不在领域模块内直接调用 provider，语义阶段统一经过 F1L RoleRuntime。

## 2. 五层验证证据

| 层 | 证据 | 结果 |
|---|---|---|
| 功能 | intake→strategy→两轮 investigator→synthesis→boundary→planner→critic→revision→approved | PASS |
| 模块 | checkpoint strict contract/tamper/unknown、SQLite CAS/reopen/tenant isolation/private permission | PASS |
| 集成 | fake RoleRuntime 六角色路由；独立 critic；GraphExecutor template；SQLite process-death resume | PASS |
| 综合 | Phase 4 全部 20 个离线门禁、Phase 3 全部 14 个回归门禁 | PASS |
| 指标/发布 | CI Phase 4 最小测试数 17→20；真实模型质量/成本/时延矩阵 | OFFLINE PASS / LIVE PENDING |

实际命令与结果：

```text
ctest --test-dir build -L phase4-cognition-v2 --output-on-failure  # 3/3 PASS
ctest --test-dir build -L phase4-offline --output-on-failure       # 20/20 PASS
ctest --test-dir build -L phase3-offline --output-on-failure       # 14/14 PASS
```

## 3. 关键负例

- checkpoint digest 被篡改、出现未知字段、CAS stale revision 或跨 tenant 加载时拒绝；
- 相同 pipeline 使用不同 TaskIntake 时以 `checkpoint_input_mismatch` 拒绝；
- LLM 字段虽存在但类型错误、claim/critic 伪造 evidence ref、计划节点无 evidence/assumption basis 时 fail closed；
- Strategy 命名未授权或非只读 investigator 时不能扩大用户 authority；
- Critic 使用 Planner 的 forbidden group/provider/model 时由 RoleRuntime independence policy 拒绝；
- clarification、HITL approval 和用户 plan revision 均为 durable stop/新 revision，不覆写历史计划；
- 模拟进程在 Synthesis 调用前后边界终止后，从最后 durable stage 恢复，已完成 investigator 不重复执行；
- terminal pipeline 重复执行不产生新的 LLM 或工具调用。

## 4. 未关闭项与边界

1. 尚未随框架发布经过真实数据集校准的 Intake/Strategy/Synthesis/Boundary/Planner/Critic/Reviser profiles、prompts 和 live provider matrix；
2. Investigator 具备生产插件接口，但 repo/docs/RAG/external adapters、来源 freshness/authority policy 和真实 ToolBus 只读沙箱尚未形成完整产品矩阵；
3. Strategy 的 `stop_conditions` 当前持久化并供审计，尚未形成可校准的自适应策略再规划器；
4. Cognition checkpoint、Evidence/Plan Store 与 Invocation Store 尚无跨 Store 原子事务；无副作用 LLM stage 可用新 attempt 恢复，但不能声称 exactly-once provider invocation；
5. `CognitionGraphTemplate` 需要显式注册，尚未成为所有 Agent 任务不可绕过的默认前置门；
6. HITL 通过确定性 validator callback 接入，尚未直接绑定 ApprovalStore 的 expiry/revocation/SoD 全语义；
7. cancellation 在 provider 调用前后协作检查，当前 LLM adapter future 不支持强制中断进行中的网络调用；
8. SQLite checkpoint 是单节点 durable store；没有远程 HA、真实故障注入、领域 benchmark 或 UI 真实截图。

因此 `R4V2-02` 保持 `partial`，成熟度估计 **75–80%**。本批证明多阶段 LLM 认知规划离线控制闭环，不证明专业任务质量、真实外部调查能力或生产 Live/SLO 已达标。
