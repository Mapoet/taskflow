# P4-V2-F7L 真实角色与 Live 认证验收报告

**日期**：2026-08-10  
**结论**：离线认证控制面 accepted；生产 Live exit gate 未关闭，`R4V2-07` 保持 partial。

## 1. 已交付

- 强类型 `LiveEnvironmentProfile`、`ProviderProfileManifest`、`RoleLiveMatrix`、`LiveCellSpec/Result`、checkpoint、signed report；
- cognition→memory→execution→assurance→judge 的必需链和 primary/critic、执行者/验证者/Judge 独立性校验；
- RoleRuntime adapter 将实际 invocation id、profile/prompt/provider/model、capability、usage、latency 和 canonical manifest digest 绑定进 cell evidence；
- assurance/Judge read-only、blind、strong oracle，memory unauthorized promotion，依赖、budget、evidence closure 和 provider/model/group independence 门禁；
- timeout、rate limit、malformed output、provider fallback、restart、cost cap 六类恢复矩阵；
- InMemory/SQLite CAS checkpoint、进程重启续跑、terminal report 原子提交；
- approval validator、可插拔 signer/verifier、expiry/revision invalidation、alert 和 GraphExecutor template；
- `AGENT_ENABLE_PHASE4_LIVE_CERTIFICATION=ON` 才注册的生产 report gate：必须有 executed/certified/无 blocker/逐 cell evidence/expected environment+matrix/未过期/HMAC 签名；缺配置返回 `BLOCKED`/exit 2。

## 2. 验证结果

| Gate | 结果 | 证明边界 |
|---|---:|---|
| `phase4-live-v2` | 5/5 PASS | strict contract、完整矩阵、负例、SQLite restart、真实 RoleRuntime invocation manifest 适配；provider 为 scripted adapter |
| `phase4-offline` | 41/41 PASS | Phase 4 离线回归，不能替代生产 provider/endpoint 证据 |
| `phase3-offline` | 14/14 PASS | Phase 3 兼容回归 |
| `phase4_live_required` 缺配置 | BLOCKED / exit 2 | no-skip 生效；不代表生产认证通过 |

## 3. 负例覆盖

- required cell 未执行或 inconclusive；
- profile/prompt/provider/model/region/manifest identity mismatch；
- verifier/Judge 非只读或非 blind、strong oracle 缺失；
- memory 越权晋升；
- provider/model/group independence 冲突；
- latency/token/cost 超限；
- failure 未恢复或 fallback 未发生；
- dependency 未通过；
- report signer/verifier 缺失或签名错误；
- environment/matrix revision 改变、报告过期、SQLite immutable binding/CAS 冲突。

## 4. 未关闭项与解除条件

当前工作区未提供以下外部事实，因此没有声称生产认证完成：

1. 批准的 production provider/model/profile/prompt/calibration/region matrix；
2. provider、IdP、MCP、A2A、Sandbox/OTLP 的真实凭据与 endpoint；
3. 部署侧 scheduled runner、KMS signing/verification 和告警通道；
4. 至少一次完整 cognition→memory→execution→assurance→judge + failure/recovery 执行；
5. 生成、审批并通过 `phase4_live_required` 的未过期 `executed=true` 报告。

以上五项具备且报告 gate 返回 0 后，才能把本报告更新为 production accepted；此前 `R4V2-07` 不得标为 complete/verified。
