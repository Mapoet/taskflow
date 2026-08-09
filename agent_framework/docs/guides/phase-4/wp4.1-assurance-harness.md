# WP4.1：Evidence-driven Assurance & Acceptance Harness

**优先级**：P0  
**依赖**：WP4.0 AcceptanceContract、WP4.2、WP4.3、WP4.9 Verification View  
**下游**：WP4.6、WP4.7

## 1. 目标与边界

判断单次任务是否真正完成。现有 Verifier 作为语义检查适配器保留，但不拥有最终裁决权。Assurance 必须通过 WP4.9 独立 Verification View 调用只读调查、测试、运行、artifact、领域和指标适配器；执行者自述、Working memory 和未验证总结只能作为待验 claim，写操作只能通过 remediation → replan → executor。

## 2. 目标模块

```text
include/agent/assurance/{types,evidence,verifier,registry,arbiter,impact}.hpp
src/assurance/{types,evidence,verifier,registry,arbiter,impact}.cpp
tests/phase4/assurance/
```

核心类型：`AcceptanceContract`、`AcceptanceCriterion`、`VerificationPlan`、`EvidenceRecord`、`VerificationFinding`、`ResidualRisk`、`AcceptanceReport`。

## 3. 可执行任务

| ID | 任务 | 交付与完成条件 |
|---|---|---|
| 4.1.1 | Acceptance schema | 五层 criterion、mandatory、oracle、threshold、evidence requirement、digest |
| 4.1.2 | Evidence provenance | source locator/digest/time/trust/freshness/claim，防重复计数和污染 |
| 4.1.3 | Verifier interface/registry | 独立 profile、Verification View/权限、输入输出 schema、timeout、inconclusive 语义 |
| 4.1.4 | Artifact inspector | 文件/二进制/数据/图像/报告/manifest 完备性和 digest |
| 4.1.5 | Code/module verifier | API、state machine、static analysis、sanitizer、fuzz/property evidence |
| 4.1.6 | Runtime/integration verifier | build/test/service/fault/recovery/real endpoint adapters |
| 4.1.7 | Domain verifier | 标准/论文/公式/数据字典/领域规则插件接口 |
| 4.1.8 | Security verifier | grants、secret、supply chain、sandbox、malicious input |
| 4.1.9 | Metric evaluator | dataset、baseline、threshold、sample、uncertainty、metric artifact |
| 4.1.10 | Legacy verifier adapter | 把 WP2.8 Verifier 降级为语义 oracle；禁止单独接受任务 |
| 4.1.11 | Conflict/freshness resolver | 高等级反例优先；过期/矛盾 → inconclusive/manual review |
| 4.1.12 | Impact graph/reverification | 修复后只重验受影响 criterion 及下游，保留历史 finding |
| 4.1.13 | Acceptance arbiter | accepted/partial/rejected/manual_review；mandatory fail-closed |
| 4.1.14 | Remediation feedback | finding → 可执行 remediation → WP4.0 controlled replan |
| 4.1.15 | Report/store | plan/artifact/evidence/verifier revisions 全绑定、可重放 |
| 4.1.16 | False-completion E2E | 测试全绿但缺产物/指标退化/文档陈旧/集成失败均被拒绝 |

## 4. Oracle 等级

确定性执行/形式约束 > 真实系统与故障注入 > 官方标准与权威数据 > 静态推导 > 校准独立模型 > 未校准模型或执行者自述。低等级证据不得覆盖高等级反例。

## 5. 五层验收

- 功能：用户可见目标、边界、负面、恢复、实际产物。
- 模块：API/算法/状态机/安全/并发/迁移。
- 集成：真实上下游、身份、deadline、一致提交、部分失败。
- 综合：需求—设计—代码—迁移—测试—文档—运行证据闭环及领域正确性。
- 指标：任务领域的质量、性能、可靠性、资源、成本和不确定度。

## 6. 安全和独立性

Verifier 默认只读；Acceptance Arbiter 无修改产物权限。执行者输出只能作为待验证 claim。Verification View 不继承私有 scratch、未验证结论或会泄漏其他 task/principal 的记录，并必须固定 evidence/memory revision。降低 mandatory criterion、改变阈值、接受高风险或把 Finding 晋升为权威项目记忆必须 WP4.3 审批并写 decision log。

## 7. DoD

至少一个跨代码、文档、运行服务和 UI/数据产物的任务经过五层验收；能够检测故意植入的总体不完备并拒绝完成；Report 可由 task/plan/artifact/evidence/memory snapshot/view revision 完整重放。

## 8. 回滚

初期以 shadow assurance 运行，不影响旧发布；误拒率达到门槛后再将 mandatory gate 设为默认。旧 Verifier gate 可回退，但回退必须标记 `legacy_unverified`，不得冒充 Phase 4 accepted。
