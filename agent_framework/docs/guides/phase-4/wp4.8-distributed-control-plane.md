# WP4.8：Distributed Control Plane

**优先级**：P2  
**依赖**：WP4.7 单机语义认证完成

## 1. 目标

在不改变已验证 Run/Plan/Acceptance/MemorySnapshot/View 语义的前提下支持多 AgentServer、多 worker、远程存储、租户配额和 HA。禁止把进程内 FIFO 直接包装成“分布式队列”。

## 2. 可执行任务

| ID | 任务 | 完成条件 |
|---|---|---|
| 4.8.1 | Storage interfaces | Run/Evidence/Artifact/Plan/Approval/Memory remote store 与 transaction/change-feed contract |
| 4.8.2 | Durable task queue | enqueue/claim/ack/nack/delay/dead-letter/idempotency/priority |
| 4.8.3 | Ownership lease | fencing token、renew/expiry、stale worker 禁止 commit |
| 4.8.4 | Worker registry | heartbeat、capability/sandbox profile、drain、version compatibility |
| 4.8.5 | Scheduler | tenant quota、公平性、priority、affinity、backpressure、admission |
| 4.8.6 | Timer/retry service | durable wakeup、shard、failover、clock/duplicate handling |
| 4.8.7 | PostgreSQL backend | migration、transaction、index、backup/restore、tenant/org/project RLS 与 Memory namespace isolation |
| 4.8.8 | Object store | artifact/evidence/memory content checksum、encryption、retention、GC/legal hold/forget |
| 4.8.9 | Capability control | distributed MCP/Skill revision publication、lease/drain/rollback |
| 4.8.10 | HA AgentServer | stateless API、SSE resume、load balancer、rolling upgrade |
| 4.8.11 | Run migration | worker loss、reclaim、sandbox reconnect/recreate、manual-review boundary |
| 4.8.12 | Chaos/load E2E | worker/server/DB/network failure、duplicate delivery、partition、scale |

## 3. 核心不变量

- 只有持有最新 fencing token 的 worker 可提交；
- queue 至少一次交付，effect 仍通过 idempotency/reconciliation；
- tenant 的 run/evidence/artifact/secret/capability 不可串读；
- Memory snapshot/index/cache 使用 fencing/generation 和 change feed，stale worker 不得提交 View 或晋升；
- rolling upgrade 不运行未知 graph/schema；
- partition 时 fail closed，不允许双 owner 无保护提交。

## 4. 五层验收

功能验证 submit/status/cancel/resume；模块验证 queue/lease/fencing；集成验证 DB/object store/sandbox/A2A；综合验证节点故障和滚动升级；指标验证吞吐、queue p95、恢复时间、fairness、duplicate claim 和资源利用率。

## 5. DoD 与回滚

任一 worker/AgentServer 退出不丢 run 或 memory/evidence revision，同一 plan node 不发生无 fencing 的并行 commit，租户/组织/项目隔离和配额在负载下成立；远程 View 在相同 snapshot/spec 下可重现。先 shadow enqueue/dual-read，再按 tenant/traffic 切换；可回退单 worker，但不得丢远程已提交状态。
