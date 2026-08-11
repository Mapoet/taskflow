# P4-V2-R7D Distributed / HA 阶段验收记录

**结论**：SQLite 与 PostgreSQL 单实例共享 durable/fencing/rolling-migration 控制面 accepted；复制后端、真实多主机部署和 HA chaos 仍为 partial。

本批将原进程内参考队列扩展为 WAL/FULL SQLite durable queue，提供 tenant-scoped idempotent enqueue、优先级 claim、lease/renew/ack/nack、dead-letter、单调 fencing token 和跨重启恢复。两个独立数据库连接的测试证明：同一任务不会同时被有效领取，lease 过期接管会提升 token，旧 worker 永远不能 ACK。

同时新增 durable worker registry 与 tenant quota：worker takeover 提升 generation，旧 instance heartbeat 被拒绝；健康窗口只返回存活 worker；并发配额跨连接原子 reserve/release 且跨重启保留。生产 scheduler API 把 claim/ACK/NACK 与 quota reserve/release 合并在同一 SQLite 事务；lease takeover 沿用原 quota slot，stale fencing token 无权完成任务或释放容量。

新增 `ObjectStore` 契约及 filesystem content-addressed adapter：tenant namespace、SHA-256 expected digest、二进制对象、幂等去重、大小限制、临时文件→fsync→rename→目录 fsync，以及读取时 size/digest 复核。测试证明相同内容跨租户物理隔离，落盘篡改会 fail closed。

rolling migration coordinator 提供 component schema version、minimum reader compatibility floor、migration lease/fencing/renew/commit；过期接管后旧 migrator 无法提交，升级 compatibility floor 后旧 reader fail-closed，状态可跨重启恢复。`phase4_distributed_chaos` 使用真实 `fork()` worker：子进程领取后直接退出，父进程在 lease 到期前不能接管，到期后以新 token 恢复，旧 token 不能 ACK 或释放 quota。

新增 `SQLiteRollingMigrationExecutor` 将该协调契约落实为真实 `prepare → expand → backfill → contract` DDL/data migration。执行计划及 SQL 全字段不可变持久化，每一阶段的应用表变更与 phase advancement 位于同一 SQLite 事务；终态 contract 同时推进 schema version/reader floor 并释放 lease。阶段调用支持响应丢失后的幂等重放，重启及 lease takeover 可用新 fencing token 恢复同一 plan，旧 token 立即失效。控制事务的 SQL 被拒绝，DDL 中途失败会同时回滚表结构、数据与 phase；租约到期、计划冲突和时间溢出均 fail-closed。

PostgreSQL 12/libpq 共享控制面已落地：`PostgresDurableQueue` 使用参数化 SQL、数据库端 `clock_timestamp()`、行锁与 `FOR UPDATE SKIP LOCKED` 实现跨连接 enqueue/claim/renew/ACK/NACK、idempotency、tenant quota 与 fencing；`PostgresWorkerRegistry` 持久化 worker generation/heartbeat；`PostgresLeaderElection` 提供数据库时间裁决的 lease/takeover/fencing。真实 PostgreSQL 测试覆盖独立连接并发领取、quota 上限、过期接管、旧 token 拒绝、`fork()` worker 无 ACK 退出，以及 backend termination 后首次操作 fail-closed、后续同对象 `PQreset` 恢复。隔离数据库经 `pg_ctl stop -m immediate` 后重启，leased task 与 fencing token 持久存在并可安全接管。

新增 `PostgresRollingMigrationExecutor`，把相同的 `prepare → expand → backfill → contract` 协议落到 PostgreSQL 事务和数据库端时钟。计划内容持久化且不可变；DDL/data mutation 与 phase CAS 在同一事务；contract 原子推进 schema version、minimum reader floor 并释放 lease。真实 PostgreSQL 测试验证重启对象后的阶段恢复、阶段幂等重放、旧 reader fail-closed、实际表重建与数据转换；故意让第二条 DDL 失败时，第一条 `ADD COLUMN` 与 phase 同时回滚。lease 到期后新 owner 获得递增 fencing token，旧 owner 无法推进 durable phase。

durable leader election 使用同一 failover 语义：双连接只能有一个 current leader，lease 到期切换提升 fencing token；旧 leader 无法 renew、release 或通过 `is_current` 写权限检查；leader 状态跨重启可审计。

新增受认证的 queue transport，将 scheduler 与 durable queue 放在真实独立服务进程边界。远程 API 只暴露 quota-aware claim/ACK/NACK，客户端不能绕过 tenant quota；enqueue 对完全相同请求幂等成功、相同 key 不同 payload fail-closed。时间裁决由服务端完成，客户端不能注入 `now_ms`；ACK/NACK 必须在租约有效期内，并在同一事务中完成状态变更和 quota release。Bearer 比较采用固定长度逐字节累积；明文 HTTP server 强制 loopback-only。

原生 mTLS 模式支持非 loopback bind，强制服务端证书、私钥、client CA，以及客户端 CA、证书和私钥全部存在；服务端要求 client certificate，客户端启用 CA 和 hostname verification，双方最低 TLS 版本固定为 1.2。`phase4_remote_queue_mtls` 在运行时生成独立 CA/server/client identity，验证可信双向认证成功、缺客户端证书在配置阶段拒绝、错误 CA 失败、主机名不匹配失败、TLS 1.0 downgrade 明确握手失败。该测试连续 10 次通过。

同一 mTLS 信任边界新增 `RemoteObjectStoreServer/Client`，客户端实现现有 `ObjectStore` 契约，服务端委托 content-addressed backend。跨进程测试覆盖含 NUL 二进制对象、expected digest 幂等重放、tenant namespace 隔离、错误 Bearer 拒绝，以及直接篡改服务端对象后远程读取的 size/digest fail-closed。该能力关闭远程对象访问适配器和传输完整性契约，但服务端当前 backend 仍是单机 filesystem，并非 S3/分布式复制存储。

`phase4_remote_queue` 使用真实 `fork()` server、TCP client 和 `SIGKILL`：验证错误凭据拒绝、进程死亡时连接 fail-closed、数据库/WAL 重启、到期前不可接管、到期后 token 单调提升、过期/旧 token 无法 ACK、新 owner 完成后 quota 精确归零。该证据关闭“进程边界 transport + server-side lease authority”，不等同跨主机 HA。

验证证据：`phase4_live_distributed`、`phase4_rolling_migration`、`phase4_distributed_chaos`、`phase4_remote_queue`、`phase4_remote_queue_mtls`、`phase4_postgres_queue` live（含 PostgreSQL 实际 rolling DDL、事务回滚与 lease takeover）、`sqlite_api_boundary` PASS；PostgreSQL crash/restart seed→verify PASS；mTLS 稳定性重复 10/10 PASS；全量重编译成功，`phase4-offline` **57/57 PASS**，`phase3-offline` **14/14 PASS**，`git diff --check` PASS。

尚未关闭：真实多主机 mTLS deployment、PostgreSQL 复制/自动故障转移、S3/分布式复制 ObjectStore backend，以及跨主机 network partition/clock-skew/leader kill chaos。PostgreSQL 单实例共享事务、rolling DDL 与进程/数据库崩溃恢复已验证，但单机 Unix socket 证据不等于复制数据库或跨主机 HA，因此 R7D 保持 partial。
