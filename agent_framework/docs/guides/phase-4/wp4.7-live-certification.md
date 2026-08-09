# WP4.7：Live Certification Matrix

**优先级**：P1  
**依赖**：WP4.4–WP4.6、WP4.9  
**下游**：生产发布、WP4.8

## 1. 目标

用受控真实依赖证明 fixture/loopback 与现实一致。Live 是独立 scheduled/manual gate，不污染无密钥默认 CI，但必须防止 skip-as-pass。

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

## 4. DoD 与回滚

至少一个 production-like 环境完成多层记忆—认知—计划—审批—sandbox 执行—五层验收全链路；证明跨 session/重启恢复固定 View、跨 org/project 不泄漏、权限轮换生效、forget 可审计，且断网、限流和服务重启均有证据。Live 失败不破坏离线开发，但阻断对应发布渠道。
