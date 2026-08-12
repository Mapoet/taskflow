# P4-GPW0–GPW10：Golden-Path Production Wiring Closure

**基线日期**：2026-08-12  
**授权**：用户批准按 GPW0→GPW10 连续实施，测试通过后自动进入下一批。
**实施状态**：2026-08-12 本地工程闭环已完成；外部 Production Live 认证不在本批伪造。

## 目标

将 GPC 的确定性完成权威从独立模块提升为所有生产入口不可绕过的运行路径。显式 demo/test 可保留 legacy AgentLoop，但必须标记为 unverified；production 缺合同、composition、coordinator、durable store 或 dependency manifest 时 fail-closed。

## 连续工作包

| ID | 交付物 | 强制验收 |
|---|---|---|
| GPW0 | profile/route/terminal 统一契约 | 未知 profile 拒绝，demo/test 显式化 |
| GPW1 | 类型化 router、诊断、审计摘要 | 缺依赖逐项 fail-closed |
| GPW2 | ProductionTaskRuntime | Harness→progress→closure 唯一链 |
| GPW3 | CLI/Web/TUI/ImGui/AgentServer wiring | production 不消费 legacy completion |
| GPW4 | GraphExecutor/Skill/ChildTask/Subflow wiring | 子任务不得推出父任务完成 |
| GPW5 | A2A 本地复验 | 远端自报 completed 拒绝 |
| GPW6 | AgentLoop model-turn outcome | guard/error/timeout/cancel 均非完成 |
| GPW7 | stage/iteration progress 与 bounded replan | 无信息增益自由轮次 ≤1 |
| GPW8 | Approval→artifact→invalidate→reverify→closure | stale approval/evidence 拒绝 |
| GPW9 | Golden A/B/C 系统与恢复安全矩阵 | mandatory cases 100% |
| GPW10 | 全回归、性能、UI、文档证据 | Phase3/4、diff、真实截图 |

## 实施结果与证据

- GPW0–2：新增 `ExecutionTrustProfile`、`ProductionClosureBinding`、`ProductionTaskRouter` 与 `ProductionTaskRuntime`。生产请求缺 binding 或依赖/组合摘要时 fail-closed；Harness checkpoint 自动生成摘要校验的 progress observation，再由唯一 `TaskClosureController` 裁决。
- GPW3：五个 LiveRuntime demo 共用 `AGENT_EXECUTION_PROFILE=demo|test|production`。CLI/Web/TUI/ImGui 的直接 React 路径在 production 明确拒绝；AgentServer 传播 profile，只有本地 closure receipt 验真后才发布 A2A `COMPLETED`。
- GPW4–6：GraphExecutor 区分 model-turn 与 task closure；ChildTask 增加 `verified_complete()`；A2A 远端自报完成必须经过本地产物、证据和 receipt 复验；guard/provider error/deadline/cancel 均保留为非完成 stop reason。
- GPW7–8：每次 durable Harness revision 投影 criteria/evidence/artifact/findings，并计算 information gain/no-progress；现有 bounded remediation、approval supersession、artifact lineage、旧证据失效与 selective reverification 接入 closure gate。
- GPW9：Golden A（文件交付）、B（失败→最小修复→复验）、C（外部依赖阻塞/恢复）以及 production missing-binding、A2A/ChildTask false-completion negative 均通过。
- GPW10：五个 demo target 全部构建；`phase4-offline` 73/73、`phase3` 22/22、定向闭环/A2A/GraphExecutor 4/4 均 PASS。此批没有视觉布局变化，UI 继续使用 GPC 已取得的真实 Web/TUI completion-state 截图证据，不以 deterministic snapshot 冒充新截图。

## 当前边界

本批关闭的是本地工程默认路径与 false-completion 信任边界，不是 R6L 外部签发。真实 IdP/KMS、多 provider、外部 scheduler、获批且未过期的 `executed=true` bundle，以及暂缓的真实多节点 chaos 仍保持开放。

## 非目标

本批不补 Gemini/vLLM/WebSocket 等外围占位，不搭建已暂缓的真实多节点 chaos，也不把本地系统测试宣称为 Production Live certification。
