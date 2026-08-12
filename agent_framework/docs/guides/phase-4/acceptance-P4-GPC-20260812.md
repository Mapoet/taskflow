# P4-GPC0–GPC10 Golden-Path Task Closure 验收

本批建立唯一任务完成权威：模型回答、guard、provider 异常、deadline 与取消只能结束模型回合；只有 `TaskClosureController` 在 Harness completion gate、mandatory criteria、artifact 和 strong evidence 全部闭合时签发 `completed_verified`。

## 已实现与证据

- TaskClosureContract、确定性终态优先级、bounded stagnation、最小修复和 fail-closed production router；
- WAL/FULL durable progress ledger，全记录摘要与重启读取；
- Golden Task A 文件交付、B 代码修复、C 外部依赖阻塞；
- Operations/Web/TUI/ImGui 显示 verified badge、authority、criteria、progress、stagnation；
- targeted 5/5、`phase4-offline` 73/73 PASS，四个 UI/demo target 编译通过；
- 真实 Web endpoint 返回 `manual_review/UNVERIFIED/task_closure_controller/8-of-9`；
- 真实截图：`/tmp/taskflow-gpc-web.png`、`/tmp/taskflow-gpc-tui.png`。

## 保留边界

这是本地 durable/golden-path 控制面证据，不等同外部 Production Live certification。所有生产入口的实际 router wiring、全 transition crash injection、真实 IdP/KMS/provider 与多主机证据仍开放，因此总体状态保持 partial。
