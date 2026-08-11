# WP4.7：Live Certification Matrix

**优先级**：P1  
**依赖**：WP4.4–WP4.6、WP4.9  
**下游**：生产发布、WP4.8

## 1. 目标

用受控真实依赖证明 fixture/loopback 与现实一致。Live 是独立 scheduled/manual gate，不污染无密钥默认 CI，但必须防止 skip-as-pass。

R6L 的执行、信任与签发口径以 [Production Live 实施计划](../phase-4-production-live-plan.md)为准。明确区分 `offline-control`、`production-like`、`production-certified`；只有最后一级能够关闭本 WP。

## 2. 可执行任务

| ID | 任务 | 完成条件 |
|---|---|---|
| 4.7.1 | EnvironmentManifest | OS/build/git/config/provider/version/endpoint class/memory policy/provider/index generation/secret refs/digest |
| 4.7.2 | Execution attestation | `executed/skipped/reason/start/end/evidence`；required job skipped=fail |
| 4.7.3 | Matrix runner | fixture/loopback/staging/production-like 分级和组合选择 |
| 4.7.4 | Real LLM | OpenAI/Anthropic/Gemini/vLLM 至少规定支持集，stream/tool/schema/retry |
| 4.7.5 | Real IdP | device flow、JWKS rotation、expiry/refresh/revoke、scope/audience/issuer |
| 4.7.6 | Real MCP | HTTP/stdio lifecycle、signature/source、credential rotation、drain/restart |
| 4.7.7 | A2A | SSE/reconnect/cancel/resubscribe/multi-agent/long task/auth rotation |
| 4.7.8 | Sandbox/renderer | container/remote、artifact、timeout/resource/cleanup、external raster |
| 4.7.9 | Failure scenarios | disconnect、429/5xx、partial memory/index write、crash、DNS/TLS、clock skew、stale/conflicting memory、forget propagation |
| 4.7.10 | Scheduled CI | secret isolation、concurrency、cost cap、artifact retention、alerts |
| 4.7.11 | Certification report | matrix cell、environment digest、evidence links、expiry、owner |

## 3. 门禁规则

Required matrix cell 必须产生 executed attestation；没有凭据、网络或 endpoint 时为 blocked/skipped，不得计为 pass。证书有有效期，provider 或关键依赖 revision 改变后自动失效。

执行器不得作为自身执行事实的唯一证明者。生产 attestation 由独立事实源核验；HMAC 仅作测试兼容，生产报告使用非对称/KMS 签名并绑定 ApprovalStore decision。

## 4. DoD 与回滚

至少一个 production-like 环境完成多层记忆—认知—计划—审批—sandbox 执行—五层验收全链路；证明跨 session/重启恢复固定 View、跨 org/project 不泄漏、权限轮换生效、forget 可审计，且断网、限流和服务重启均有证据。Live 失败不破坏离线开发，但阻断对应发布渠道。

## 5. 2026-08-10 实施状态

F7L 已补齐 4.7.1–4.7.3、4.7.9–4.7.11 所需的通用控制面：typed environment/profile/matrix/cell/report、严格反序列化、RoleRuntime manifest adapter、required no-skip、SQLite CAS/restart、六类恢复、审批/签名/到期/版本失效/告警、GraphExecutor 和显式生产 report gate。`phase4-live-v2` 5/5 PASS 只证明控制面和 scripted RoleRuntime 集成。

4.7.4–4.7.8 的真实 LLM/IdP/MCP/A2A/Sandbox/renderer 组合尚未执行；4.7.10 的部署侧 scheduled runner/KMS/secret/alert 配置也未提供。生产门禁只有在 `AGENT_ENABLE_PHASE4_LIVE_CERTIFICATION=ON` 时注册，缺任何报告或绑定参数即 `BLOCKED`/exit 2。当前没有批准且未过期的 `executed=true` 生产报告，因此本 WP 保持 partial。
