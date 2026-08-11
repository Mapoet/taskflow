# P4-V2-R6L Production Live Certification 阶段验收记录

**结论**：R6L-R0/R1/R3/R4 软件信任边界 accepted；外部 production-certified 矩阵尚未执行，R6L 仍为 blocked，不能签发生产认证。

此前 `LiveCellExecutor` 的 `executed=true`、manifest/evidence digest 和 role identity 由 executor 返回，最终报告签名只能证明报告内容未被后改，不能独立证明这些 digest 对应真实外部调用。本批在 `RoleCertificationOptions` 增加独立 `cell_evidence_verifier`：production environment 缺 verifier 时报告强制 `Inconclusive`；任一已执行 cell 的 environment/spec/result attestation 校验失败时加入 blocker。离线 deterministic verifier 只是控制面 fixture，生产实现必须查询不可变 Invocation/Audit/Artifact/Oracle 事实源或验证外部签名。

后续 `phase4-r6l-production-live-r1` 已冻结三档证据口径和 mandatory matrix。新增严格 `ProductionLiveBundle`，拒绝 unknown field、缺失 mandatory cell/dependency、非 opaque secret reference 和 production level/environment 不一致。新增 WAL/FULL `SQLiteProductionAttestationStore` 与 `StoreBackedCellEvidenceVerifier`：attestation 按 tenant+invocation append-only，跨重启从独立 Store 核对 environment/matrix/spec/invocation manifest/result/evidence/oracle digest；重复写、结果篡改和跨环境重放全部拒绝。

生产签发从共享 HMAC 升级为 Ed25519 公钥验证；HMAC 仅保留历史 fixture 口径。required gate 新增 expected Approval decision、SQLite ApprovalStore、tenant/scope/time 验证。`StoreBackedProductionApprovalVerifier` 校验 request canonical digest、approved 状态、requester/reviewer SoD、policy revision、scope、expiry，以及 request/decision 对 report signing digest 的双重绑定。

新增 `ProductionLiveRunner` 统一装配 production bundle、durable certification checkpoint、独立 attestation verifier、ApprovalStore 与 signer。它仅接受 `production-certified` bundle；首次真实执行生成 `AwaitingApproval` 候选报告，`production_approval_signing_digest` 计算绑定 approval ID 的拟签发摘要，accountable decision 落盘后，同一 workflow 从 durable checkpoint 恢复，不重复已执行 cell，并在重新核验 evidence/approval/signature 后进入 Certified。

验证证据：`phase4_production_bundle`、`phase4_production_attestation`、`phase4_production_approval`、`phase4_production_signature`、`phase4_production_runner` 全部 PASS；开启 `AGENT_ENABLE_PHASE4_LIVE_CERTIFICATION=ON` 后 required gate 构建成功，缺生产输入返回 `BLOCKED`/exit 2；全量构建 PASS，`phase4-offline` **62/62 PASS**，`phase3-offline` **14/14 PASS**，`git diff --check` PASS。

`role_certification_valid_for` 同样要求 production report 提供 cell evidence verifier，避免已落盘报告在发布门禁处绕过二次证据核验。负例证明：同一份签名有效且 blocker-free 的 fixture report，在缺少 cell verifier 时仍然无效；workflow 缺 verifier 时生成 `cell_evidence_verifier_missing` blocker。

验证：`phase4-live-v2` 5/5（首次并行构建后遇到一次 Linux `text file is busy`，目标稳定后单独重跑通过）；最新全量重编译通过 `phase4-offline` **56/56**、`phase3-offline` **14/14**、`sqlite_api_boundary` 与 `git diff --check`。该证据证明 fail-closed 控制面和离线回归稳定，不证明真实 provider/IdP/MCP/A2A/Sandbox/renderer/KMS 已执行。

当前只检测到单一 Anthropic 兼容 credential/base/model 变量组；没有第二独立 provider、IdP/MCP/A2A endpoints、生产 ApprovalStore 决策或 KMS/private signer 配置。解除 R6L blocked 的充分条件仍是：提供获批环境 manifest、完整 role/dependency matrix、opaque credentials、真实 external adapters、accountable approval、KMS/asymmetric signer 和 scheduled runner，得到未过期、所有 mandatory cell executed/pass、cell attestation 有效、无 blocker 的生产报告。
