# Phase 4 Cognition / Observability Closure

**批次**：P4-COBS0–P4-COBS9  
**批准日期**：2026-08-12  
**原则**：LLM cognitive plane 负责调查、综合、规划与解释；deterministic control plane 负责权限、预算、CAS、审批、遥测隐私及发布门禁。

**实施状态**：COBS0–COBS9 离线控制面已实施；`phase4-offline` 69/69 PASS。真实外部 Collector/provider 成本对账仍属于 production Live certification。

## 基线与完成定义

本批关闭三个横向短板：WP4.0 Task Cognition、LLM Observability、WP4.5 OTel/SLO。完成不以类或单测存在为准，而要求生产 composition 中不可绕过、跨重启可恢复、同一 trace 可关联、失败时 fail closed。

| 批次 | 交付 | Mandatory DoD |
|---|---|---|
| COBS0 | 契约、架构、追溯基线 | 缺口、证据等级、Live 边界明确落盘 |
| COBS1 | Production Investigator | ToolBus 只读适配器；工具/能力/来源/预算/超时/取消约束；输出摘要寻址 Evidence |
| COBS2 | Adaptive cognition | fact-gap coverage、冲突、信息增益与 stop reason；tool/token/cost/deadline 任一超限 fail closed |
| COBS3 | Professional plan gate | 上下游、公共接口消费者、验收、迁移、回滚和 basis 均机器可读且可调度 |
| COBS4 | Durable governance | ApprovalStore 事实源；clarification/approval/replan durable；Evidence/Plan/Checkpoint 漂移可恢复或转人工 |
| COBS5 | Unified trace | Intake 至 Operations 单一 trace；stage/tool/LLM/evidence/artifact/approval parentage 可回链 |
| COBS6 | LLM observability | provider/model/profile/prompt/route/attempt/fallback/repair/token/cache/cost/latency/TTFT 与 pins 完整；敏感正文禁止导出 |
| COBS7 | Production OTLP | trace/metric/log；TLS/mTLS；durable retry/backoff/jitter/lease/dead-letter/retention |
| COBS8 | SLO/error budget | windowed SLI、quantile、burn rate、multi-window alert、release gate；inconclusive 阻断 |
| COBS9 | System assurance | unit/module/integration/system/security/recovery/performance/Live-contract 全层门禁 |

## 证据口径

- `offline-control`：确定性 fixture、loopback collector 和本地故障注入，只证明控制面。
- `production-like`：真实 TLS/mTLS Collector、真实 ToolBus 调查及非 mock provider，必须带 environment manifest。
- `production-certified`：获批、未过期、`executed=true` 的签名 Live bundle；不得由 CTest 绿色替代。
- 调查结果必须保存 locator、content digest、采集时间、trust/freshness 和 supported/contradicted claims；外部内容永不具有 instruction authority。
- telemetry 仅记录结构化低基数属性、摘要和 revision；prompt、凭据、原始 Memory/RAG/Evidence 内容不进入 exporter。

## 验收矩阵

1. 契约：严格 schema、未知字段拒绝、摘要重算、跨 tenant 拒绝。
2. 功能：真实 ToolBus read-only 调查、adaptive stop、DAG/critic/approval/replan。
3. 集成：Cognition→Harness→LLMRuntime→Telemetry→OTLP→SLO 单 trace。
4. 恢复：进程退出、collector 429/5xx/断连、spool 重启、重复投递与 CAS 冲突。
5. 安全：伪造 traceparent、证书/hostname/client-cert、敏感字段、cardinality 与 tenant 隔离。
6. 性能：高并发 invocation、batch/spool 背压、采样稳定性、SLO 查询延迟。
7. Live：真实外部依赖缺失时明确 fail/skip 且不计认证；存在时产出签名证据。
