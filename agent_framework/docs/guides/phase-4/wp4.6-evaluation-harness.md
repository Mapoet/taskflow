# WP4.6：Evaluation Harness

**优先级**：P1  
**依赖**：WP4.0、WP4.1、WP4.5、WP4.9  
**下游**：发布和 WP4.7

## 1. 目标

评估 planner/memory view/executor/verifier/arbiter/model/prompt 的总体质量和版本退化。它运行任务集合，不替代单次任务 Assurance。

## 2. 可执行任务

| ID | 任务 | 完成条件 |
|---|---|---|
| 4.6.1 | Dataset schema | task/input/environment/ground truth/criteria/tags/license/version/digest |
| 4.6.2 | Trajectory schema | model/prompt/tool/plan/memory snapshot/view/revision/evidence/finding/usage/timing |
| 4.6.3 | Eval runner | deterministic seed、parallelism、resume、isolation、artifact output |
| 4.6.4 | Dataset registry | immutable revision、split、防泄漏、tenant/private dataset |
| 4.6.5 | Planning metrics | fact citation、dependency omission、unexecutable/over-authority/replan |
| 4.6.6 | Execution metrics | task success、tool precision、invalid/repeated calls、effects、cost/latency |
| 4.6.7 | RAG/memory metrics | scope leak、mandatory retention、false memory、conflict/stale detection、recall@k/MRR/nDCG、citation faithfulness、view reproducibility、compaction retention、token efficiency、task success delta、promotion/forget |
| 4.6.8 | Assurance metrics | defect recall、false reject、evidence sufficiency、false-completion block |
| 4.6.9 | Judge framework | prompt/version、calibration set、agreement、bias、abstention |
| 4.6.10 | Comparison/statistics | baseline/candidate、confidence interval、paired test、effect size |
| 4.6.11 | Flaky/cost gate | repeat policy、variance、budget、regression threshold、quarantine |
| 4.6.12 | CI/report | small offline PR set、nightly full matrix、machine JSON + human report |
| 4.6.13 | Domain adapters | 软件、RAG、UI、科研数值/数据产品至少各一 fixture |

## 3. 防污染原则

Dataset ground truth 不进入 executor 或 Memory View；judge 不访问 candidate label；eval tenant/project 与被测记忆严格隔离，测试结束清理派生索引和 candidate。模型供应商、参数、Prompt、tool/capability/memory policy revision 全记录。人工标注必须有 guideline、reviewer 和 disagreement。

## 4. 五层验收

功能验证 runner/report；模块验证 metric/statistics；集成验证真实 planner/assurance/sandbox；综合验证版本升级门禁；指标验证 eval 自身复现率、judge agreement、false positive/negative 和成本。

## 5. DoD 与回滚

任一关键版本变更都有固定数据集、基线和统计门槛；至少证明一个规划退化和一个记忆污染/跨 scope 退化会阻断发布。Eval gate 初期 advisory，稳定后逐项 mandatory；不得删除失败样本或把 eval ground truth 写入长期记忆来“修复”回归。
