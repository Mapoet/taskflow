# P4-V2-F6E Judge、评测与校准离线核心验收报告

**批次**：`P4-V2-F6E-OFFLINE-CORE-20260810`  
**计划**：`P4-V2-F6E.1–F6E.9 / phase4-v2-plan-r1`  
**结论**：离线控制面 accepted；`R4V2-06` 保持 partial

## 已实现范围

- 新增版本化 `EvaluationSuite`、`CandidateEvaluationRun`、`JudgeBatch`、`JudgeCalibrationReport`、`QualityMetricReport`、`UpgradeDecision`、`EvaluationReport` 与 `JudgeCheckpoint` typed contracts；嵌套字段采用 closed schema，canonical digest 参与恢复和发布绑定。
- suite 支持 unit、repository、domain、adversarial、recovery、live 六层 case；run 必须绑定 tenant/task、dataset version、criteria、trajectory、执行时间和有限数值指标。live 层无实际执行时间戳时 fail closed。
- 新增隔离的 Evaluation Memory View：只允许 System/Organization 层的 Instruction/Procedural/Evidentiary，关闭 procedural skill 注入并排除未验证 executor claim。
- Blind orchestration 对每个 case 用固定 seed 生成 `artifact-A/B` 映射，Primary/Secondary 使用反向展示顺序。Judge 能看到题面、环境、criterion 和匿名产物，但看不到 ground truth、baseline/candidate revision、profile、prompt、provider 或 model 身份；受保护字段递归检查失败时不调用模型。
- Primary、Secondary 和争议 Adjudicator 均通过 `JudgeStageModel`；生产适配器 `RoleRuntimeJudgeModel` 固定 Evaluation View 和 `phase4-v2-f6e-r1` policy revision。Secondary/Adjudicator 受 provider、model、independence-group 隔离门禁约束。
- 模型输出采用严格 verdict schema：exact case coverage、合法 alias、artifact/criterion score `[0,1]`、confidence、evidence refs 和 risks。未知字段、未知 case、缺失 criterion、重复 verdict、非有限值和标签化 alias 均拒绝。
- 确定性校准计算 raw agreement、Cohen's kappa、first-position win rate、candidate-baseline score variance、hidden ground-truth accuracy、candidate win rate 与 Wilson 95% CI；争议样本进入独立仲裁，同时低 agreement 仍触发 manual review。
- 指标目录覆盖规划、记忆、Assurance、Remediation 和 Runtime 共 24 项，包括 evidence citation、plan executability、scope leakage、false accept/reject、impact/reverify precision、latency/token/cost/retry/fallback/recovery/budget compliance。按 case id 配对，检测 missing、flaky、critical-zero、minimum candidate 和统计 regression。
- Upgrade gate 不接受 LLM 直接发布：确定性 regression 或 critical violation 必须 Reject 并指向 baseline rollback；缺指标、flaky、低 agreement 或低校准准确率进入 ManualReview；通过后仍由 PDP/HITL approval digest 决定是否 Approved。
- checkpoint、Judge output/InvocationManifest 摘要、token/cost、匿名映射、校准、指标和 decision 使用 SQLite CAS 持久化；终态 checkpoint 与 EvaluationReport 单事务提交，等待审批后可重启恢复且不重复调用 Judge。
- `JudgeGraphTemplate` 将同一 durable workflow 接入 GraphExecutor；报告携带 `run_kind`、`trend_key`、`executed` 和完整 input digest chain。

## 验证证据

| 层级 | 证据 |
|---|---|
| 契约/持久化 | `phase4_judge_contracts`：typed round-trip、outer/nested unknown field、tamper、InMemory/SQLite CAS、immutable binding、重开恢复 |
| 功能/指标 | `phase4_judge_workflow`：匿名输入、双 Judge、24 项指标、agreement/ground-truth calibration、approval/rollback；争议仲裁后低 agreement→ManualReview |
| 策略负例 | `phase4_judge_negative`：artifact 身份泄漏零调用、非法 alias bounded retry→ManualReview、LLM 偏好 candidate 但确定性 metric regression→Rejected/rollback |
| 集成/恢复 | `phase4_judge_integration`：两个真实 RoleRuntime route 与 Evaluation View、独立 provider/model；SQLite AwaitingApproval→重启→Approved、terminal report 原子恢复、GraphExecutor |
| 综合 | `phase4-judge-v2` 4/4 PASS；`phase4-offline` 36/36 PASS；`phase3-offline` 14/14 PASS |

## 证据边界与残余风险

本批次冻结的是评测控制面，不是生产模型校准结论。尚未关闭：

- 当前 dataset 和 Judge 为离线 fixture，没有真实仓库、GNSS/领域、对抗、recovery 或 live case，也没有人工双标注与 inter-rater agreement；
- 未对生产 provider/model/profile/prompt revision 建立足量样本、bootstrap/分层 CI、drift 与长期 false accept/false reject 基线；
- nightly scheduler、报告加密签名/证明链、历史 trend store、SLO/告警和自动 rollback executor 未实现；
- suite/run/report、Invocation、Approval、Artifact 与发布 registry 尚未跨 Store 单事务协调；
- 默认生产入口尚未证明不可绕过 F6E gate，live matrix 和 failure injection 也未执行；
- LLM Judge 始终是 advisory evidence，不能覆盖测试、artifact、数值、security 或外部副作用等强 oracle 反例。

因此 F6E.1–F6E.8 的离线控制面和 F6E.9 的 report/resume/flaky 子集可接受，但 F6E.9 的 nightly/signature/trend/SLO 以及整个 `R4V2-06` 的生产 DoD 不得标记 complete。
