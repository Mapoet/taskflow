# P4-V2-F1L Role-based LLM Runtime 验收报告

**日期**：2026-08-09  
**计划**：`phase4-v2-plan-r1 / P4-V2-F1L.1–F1L.12`  
**授权**：用户明确批准实施 P4-V2-F1L  
**结论**：离线核心 `accepted`；完整 F1L `partial`，不得声称 production complete

## 1. 已验收范围

- 六类 typed contracts：Role Profile、Prompt Revision、Route Decision、Invocation Manifest、Reasoning Artifact、Calibration Record；统一 canonical digest、strict unknown-field 和 tamper rejection；
- immutable Profile/Prompt/Calibration registry，以及 tenant 隔离的 InMemory/SQLite store；
- capability、Memory View、residency、context、cost、availability、provider/model/group independence 的确定性路由与 fail-closed negative path；
- strict JSON Schema output gate、bounded repair、retry/fallback、request-scoped model config；
- Pending→Running→terminal CAS manifest、attempt/usage/cost/latency/fallback/output digest、重复 invocation 幂等拒绝；
- 重启后 Running 调用转 ManualReview，避免对 provider 不确定结果进行盲目重放；
- Telemetry/Audit correlation；不持久化 provider 私有 reasoning；结构化结果在 schema 接受前不向调用方流出；
- Role Profile 拒绝 credential-like extra params，OpenAI/Anthropic adapter 拒绝由 profile 覆盖 model/messages/system/tools/stream/采样与预算等受保护请求字段；
- AgentLoop 显式注入 RoleRuntime 的兼容 adapter，并传递 profile pin、Memory View pin、region 与 granted capabilities；legacy 路径保持可回滚。

## 2. 五层验证证据

| 层 | 证据 | 结果 |
|---|---|---|
| 功能 | profile/prompt render、route、schema accept/repair、usage aggregation | PASS |
| 模块 | 6 契约 round-trip/tamper/unknown；router policy negatives；SQLite immutable/CAS/restart | PASS |
| 集成 | fake-primary 503 → fake-fallback malformed JSON → bounded repair → accepted output；Telemetry/Audit/Store 一致 | PASS |
| 综合 | `phase4-offline` 17 个门禁；Phase 3 14 个回归门禁 | PASS |
| 指标/发布 | CI Phase 4 最小测试数由 13 提升至 17；live provider 指标不在本报告接受范围 | OFFLINE PASS / LIVE PENDING |

实际命令与结果：

```text
ctest --test-dir build -L phase4-llm-runtime --output-on-failure  # 4/4 PASS
ctest --test-dir build -L phase4-offline --output-on-failure     # 17/17 PASS
ctest --test-dir build -L phase3-offline --output-on-failure     # 14/14 PASS
ctest --test-dir build -R '^(llm_client_wp1|agent_loop_guard_wp16|agent_loop_wp5|agent_loop_tool_parallel_wp21b|agent_loop_context_budget_i1_wp21c|skill_agent_loop_policy_contract)$' --output-on-failure  # 6/6 PASS
```

## 3. 关键负例

- canonical digest 被篡改或出现未知字段时拒绝解码；
- capability、Memory View、region、cost、independence evidence 不满足时不调用 provider；
- 未批准 calibration 或 fallback model 不匹配 calibration 时拒绝；
- credential-like provider 参数以及企图覆盖 route/prompt/tool/stream/采样/预算的受保护字段被拒绝或忽略；
- malformed structured output 不泄露，修复预算耗尽则失败；
- 重复 invocation ID 不产生第二次 provider side effect；
- restart 后 Running 状态不自动重放，进入人工复核；
- manifest/output digest 不包含测试注入的私有 reasoning 字符串。

## 4. 未关闭项与边界

1. 规划、记忆、专业验证、Judge 尚未全部迁入 RoleRuntime，仍不能证明“任何角色均不可绕过”；
2. `StructuredOutputPolicy` 暂内嵌 Prompt revision，独立第七契约及迁移 fixture 待关闭；
3. Profile registry 尚无 project/org overlay、default pointer/active revision 管理；回滚当前依赖调用方重新 pin；
4. AgentLoop 有显式 adapter，但 GraphExecutor 没有强制 feature gate；
5. 尚无可重复的 timeout/deadline fault fixture、真实 OpenAI/Anthropic route、真实 usage/cost 对账与 role calibration live matrix；
6. SQLite 是单机 durable store，不是跨节点 HA/远程事务存储。

因此 `R4V2-01` 记为 `partial`，成熟度估计 70–75%。上述未关闭项必须进入后续 F2C/F7L 门禁或 F1L residual closure，不能由本次离线绿色测试推断为已完成。
