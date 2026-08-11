# P4-V2-R4O Sandbox / OTLP / SLO 增量验收报告

**状态**：partial（deny-all production profile accepted）  
**日期**：2026-08-11

## 已实现并直接验证

- `BubblewrapSandboxProvider`：Linux `unshare+bwrap` 隔离，PID/IPC/UTS/network namespace，显式只读/可写 mount，CPU、地址空间和墙钟限制，进程组超时清理；
- `CredentialBroker`：只接受 opaque reference，拒绝 raw/duplicate reference；秘密通过匿名管道进入 `/run/secrets`，不进入 argv/env，stdout/stderr 在返回与 digest 前脱敏；
- workspace：symlink fail-closed、quota、稳定输入/输出 digest，以及 added/modified/removed diff digest；
- telemetry：严格 W3C `traceparent` 校验与格式化，OTLP/HTTP JSON span/metric batching，跨进程 loopback collector；
- telemetry durability：SQLite at-least-once spool 在 downstream/flush 失败时保留记录，进程重启后按 sequence 重放，ACK 后事务删除；
- audit bridge：成功导出的 span/metric 生成已脱敏、带 payload digest 的 AuditEvent；
- SLO：按样本数和加权观测值评估，缺样本为 `inconclusive`，阈值失败和 inconclusive 均可阻断 release。
- Harness adapter：`SandboxExecutionHarnessPort` 将 manifest/output/receipt digest 回填 Harness pin/outbox；SQLite receipt journal 提供 idempotent replay 与跨进程 reconcile，receipt 缺失的 crash window 保持 ManualReview。

## 负向与集成证据

- `phase4_process_sandbox`：真实 bwrap 执行、credential 泄漏为 0、workspace 输出 digest/diff、handle 销毁、超时 kill、非空网络 allowlist fail-closed；
- `phase4_otlp_loopback`：独立子进程向父进程 loopback collector 提交 `/v1/traces` 与 `/v1/metrics`，collector 验证 trace ID 和 metric name；
- `phase4_runtime_observability`：invalid traceparent、raw/duplicate credential、telemetry allowlist、Audit bridge、SLO insufficient/pass/fail；
- 同一测试覆盖 telemetry spool 的失败保留、重启恢复、成功 ACK 清空；
- `phase4_sandbox_harness`：同一 idempotency key 单次 effect、进程内 replay、SQLite 重启 reconcile；
- `phase4-offline` 52/52 PASS，`phase3-offline` 14/14 PASS，R4O targeted 4/4 PASS，`git diff --check` PASS。

## 尚未关闭的边界

1. `bubblewrap-v1` 的网络能力是可证明的 deny-all。非空 domain/CIDR allowlist 会明确拒绝；尚未交付 egress proxy/DNS pinning，因此不能宣称细粒度网络 allowlist 已实现。
2. receipt journal/reconcile 已接入 Harness；但 sandbox handle 仍是进程内控制对象，effect 完成但 receipt 尚未提交的 crash window 会正确进入 ManualReview，尚不能自动证明或回收孤儿进程。
3. OTLP exporter 已验证跨进程 HTTP loopback并具备 SQLite spool；尚未验证真实 OpenTelemetry Collector、TLS/mTLS 和调度型指数 backoff。
4. SLO gate 已 fail-closed，但尚未装配到默认 production Harness release composition。

因此 R4O 维持 partial；上述四项必须在进入最终 Phase 4 acceptance 前有直接证据。
