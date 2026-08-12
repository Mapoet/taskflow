# Phase 4 Professional Assurance / Production Oracle Closure

**批次**：P4-PAO0–P4-PAO9  
**批准日期**：2026-08-12  
**状态**：offline-control verified；production deployment/Live partial

## 目标与证据边界

本批关闭当前最短板：专业验收不能只消费 fixture observation 或 LLM 自报结论，生产路径必须执行摘要绑定、来源明确、受控且不可绕过的真实 Oracle。LLM 继续负责 Verification Planning、五角色专业解释和 Evidence Resolution；文件、产物、build、test、runtime、metric、security/domain 检查及最终 Arbiter 属于 deterministic control plane。

`phase4-offline` 只证明控制面。真实外部领域系统、生产 provider、IdP/KMS/Collector 缺失时保持 `partial/blocked/inconclusive`。

## 连续工作包

| 批次 | 交付与退出门槛 |
|---|---|
| PAO0 | 三个事实源、trace/ledger 去除 SIC 陈旧缺口；冻结证据等级 |
| PAO1 | Oracle production readiness、source kinds、revision/capability manifest |
| PAO2 | repository/file/artifact completeness 真实只读 Oracle，路径越界拒绝 |
| PAO3 | Bubblewrap build/test/runtime/metric command Oracle；deny-all 网络、资源/超时、manifest receipt |
| PAO4 | policy-bound domain/security SPI；缺 mandatory adapter fail closed |
| PAO5 | Production Assurance/Reverification 强制 production oracle coverage |
| PAO6 | artifact revision/freshness 感知失效、forced rerun、lineage 与选择性复用 |
| PAO7 | mandatory evidence strength、独立 verifier、deterministic Arbiter/误接受门禁 |
| PAO8 | contract/module/integration/system/security/recovery/performance 七层测试 |
| PAO9 | full Phase 4 regression、trace/ledger/status；外部 Live 边界校准 |

## 强制不变量

- `ManifestEvidenceOracle` 只允许 offline/导入可信 observation；`production_ready=false`。
- 每个 mandatory criterion 的每类 `required_evidence` 必须至少有一个 production-ready Oracle。
- Production workflow adapter 始终启用 `require_production_oracles`，部署方不能通过 input options 关闭。
- Oracle capability manifest 固定 provider/adapter revision、policy revision、规则与 source kinds。
- 文件路径必须是 workspace 内相对路径；命令只在 SandboxProvider 执行，网络默认 deny-all。
- Calibrated/uncalibrated LLM 意见不能单独满足 mandatory criterion；强反证优先。
- artifact digest、evidence freshness 或 policy revision 改变时，旧 evidence 不得静默复用。

## 验收矩阵

1. Contract：空 revision、空规则、缺 source、未知 criterion、非法路径、摘要篡改。
2. Module：存在/非空/digest、exit code、timeout/resource、provider unavailable、domain adapter missing。
3. Integration：Production Assurance adapter 强制 coverage；offline Manifest Oracle 无法冒充生产。
4. System：不完整产物拒绝→受批修复→新 artifact→强制复验→仅强证据闭合后接受。
5. Security：路径逃逸、网络越权、凭据/输出泄漏、producer self-claim、低强度意见误接受为 0。
6. Recovery：Assurance checkpoint、Sandbox effect、artifact lineage、重复投递与 restart。
7. Performance：规则规模、输出/存储背压、wall-time、低基数字段与 SLO。

## PAO9 验证结果

- 定向：`phase4_assurance_workflow`、`phase4_production_workflow_adapters`、`phase4_production_oracles`，3/3 PASS。
- 全量：2026-08-12 `phase4-offline` **71/71 PASS**。
- 安全/系统：真实 Bubblewrap read-only workspace command、路径约束、缺 source fail-closed、低强度 LLM mandatory 误接受拒绝。
- 格式：`git diff --check` PASS。
- 未认证：具体生产五层规则、外部 domain/security adapter、真实 provider calibration、IdP/KMS/Collector 与签名 Live bundle。
