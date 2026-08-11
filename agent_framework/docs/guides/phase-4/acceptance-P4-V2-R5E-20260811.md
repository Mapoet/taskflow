# P4-V2-R5E Dataset / Calibration / Nightly 验收记录

**结论**：治理与持久化控制面 accepted；真实生产数据、人工标注活动、生产模型校准和外部 scheduler 仍为 partial。

本批新增六层数据集 manifest（unit/repository/domain/adversarial/recovery/live）的完整性、版本和 case digest 绑定；双 reviewer 人工标签及 Cohen’s kappa 基线；flaky quarantine（持续失败不得被隔离掩盖）；不可变 SQLite campaign/manifest/label/quarantine/report 存储；campaign lease；固定 baseline/candidate/model/prompt/profile/suite digest；签名摘要验证、防篡改和跨重启趋势比较。

验证证据：`phase4_eval_governance` 覆盖缺层、篡改 case digest、重复 reviewer、签名篡改、不可变冲突、租约争抢与过期接管、重启恢复和 trend delta；`sqlite_api_boundary` 证明新增实现只使用 `agent/internal/sqlite_utils.hpp`。全量 `phase4-offline` 为 **53/53 PASS**，`phase3-offline` 为 **14/14 PASS**。

尚未关闭：仓库发布的真实六层样本与许可证审查、两名领域专家的实际标注结果、真实 provider/profile/prompt/model revision 校准、部署 scheduler/KMS 签名、长期 false-positive/false-negative 与 SLO 趋势。因此 R5E 只能从 planned 提升为 partial，不能宣告生产质量门禁完成。
