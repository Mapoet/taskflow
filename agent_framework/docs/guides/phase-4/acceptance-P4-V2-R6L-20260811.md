# P4-V2-R6L Production Live Certification 阶段验收记录

**结论**：生产证据门禁已进一步加固；外部生产矩阵尚未执行，R6L 仍为 blocked，不能签发生产认证。

此前 `LiveCellExecutor` 的 `executed=true`、manifest/evidence digest 和 role identity 由 executor 返回，最终报告签名只能证明报告内容未被后改，不能独立证明这些 digest 对应真实外部调用。本批在 `RoleCertificationOptions` 增加独立 `cell_evidence_verifier`：production environment 缺 verifier 时报告强制 `Inconclusive`；任一已执行 cell 的 environment/spec/result attestation 校验失败时加入 blocker。离线 deterministic verifier 只是控制面 fixture，生产实现必须查询不可变 Invocation/Audit/Artifact/Oracle 事实源或验证外部签名。

`role_certification_valid_for` 同样要求 production report 提供 cell evidence verifier，避免已落盘报告在发布门禁处绕过二次证据核验。负例证明：同一份签名有效且 blocker-free 的 fixture report，在缺少 cell verifier 时仍然无效；workflow 缺 verifier 时生成 `cell_evidence_verifier_missing` blocker。

验证：`phase4-live-v2` 5/5（首次并行构建后遇到一次 Linux `text file is busy`，目标稳定后单独重跑通过）；最新全量重编译通过 `phase4-offline` **56/56**、`phase3-offline` **14/14**、`sqlite_api_boundary` 与 `git diff --check`。该证据证明 fail-closed 控制面和离线回归稳定，不证明真实 provider/IdP/MCP/A2A/Sandbox/renderer/KMS 已执行。

解除 R6L blocked 的唯一充分条件仍是：提供获批环境 manifest、opaque credentials、真实 external adapters、KMS signer/verifier 和 scheduled runner，得到未过期、所有 mandatory cell executed/pass、cell attestation 有效、无 blocker 的生产报告。
