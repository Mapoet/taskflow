# WP4.3：Durable HITL & Policy Engine

**优先级**：P0  
**依赖**：WP4.2  
**下游**：WP4.0、WP4.1、WP4.4、WP4.9

## 1. 目标

把 ManualReview 和临时用户询问统一为持久 interruption；对澄清、计划、工具参数、副作用、验收阈值、风险接受、记忆晋升/纠错/遗忘执行同一政策与审计协议。

## 2. 可执行任务

| ID | 任务 | 完成条件 |
|---|---|---|
| 4.3.1 | Approval schema | request/decision/reviewer/scope/reason/expiry/policy revision/digest |
| 4.3.2 | PolicyDecisionPoint | allow/deny/require approval/modify，确定性规则优先 |
| 4.3.3 | PolicyInformationPoint | tenant/org/principal/project/task、role/tool/effect/risk/plan/criterion/memory/sandbox context |
| 4.3.4 | Durable interruption bridge | Graph node 暂停、RunStore 保存、resume token 单次消费 |
| 4.3.5 | Clarification flow | 事实不清时询问；回答成为 EvidenceRecord，触发 replan |
| 4.3.6 | Plan approval | approve/reject/edit node/限制 authority；plan digest 防 TOCTOU |
| 4.3.7 | Tool/MCP approval | 参数 diff、effect class、credential/sandbox scope |
| 4.3.8 | Acceptance/risk/memory approval | 禁止静默降低 mandatory criterion；风险接受有期限；跨层/权威记忆写入、纠错和 forget 有 scope/diff |
| 4.3.9 | Delegation/two-person | reviewer role、delegate、separation of duties、双人规则 |
| 4.3.10 | Expiry/revocation | 过期、撤回、task cancel、policy revision 变化后重新评估 |
| 4.3.11 | CLI/Web/TUI model | pending list、详情、证据、diff、approve/reject/edit；真实截图 |
| 4.3.12 | Audit/E2E | 跨进程、部分审批、并发决定、重放攻击、越权 reviewer |

## 3. 安全不变量

- decision 绑定 request、plan、arguments、artifact 和 policy digest；
- resume token 单次使用且不可跨 tenant/task；
- 修改参数后必须重新校验 schema/policy；
- hosted/remote approval 不能泄露 secret；
- verifier/arbiter 不得审批自己的阈值降低；
- memory consolidator 不得审批自己的跨层晋升，approval 必须绑定 candidate/current/target revision 与 View provenance；
- deny/expired/revoked 永远不能因恢复失败变成 allow。

## 4. 五层验收

功能覆盖 approve/reject/edit；模块覆盖状态机/签名/expiry；集成覆盖 ToolBus/MCP/RunStore/UI；综合覆盖长时间审批和 plan rebase；指标覆盖审批延迟、越权拦截率、重复执行率和 pending backlog。

## 5. DoD 与回滚

审批可跨进程和跨时间恢复；未批准副作用和权威记忆晋升绝不执行；阈值、记忆纠错与遗忘传播可审计。初期 policy 仅对新 Phase 4 workflow 强制，legacy 路径标记无 durable approval，不能宣称 Phase 4 compliant。
