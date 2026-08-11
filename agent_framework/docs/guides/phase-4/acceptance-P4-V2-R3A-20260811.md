# P4-V2-R3A Approval / Memory / Operations 集成验收报告

**状态**：partial（integrated core accepted）  
**日期**：2026-08-11

## 已直接验证

- accountable approval：reviewer identity/role、SoD、delegation scope、高风险双人复核、vote CAS、SQLite restart reconciliation、edit 新 revision、expiry/stale/duplicate 拒绝；
- Harness：真实 ApprovalStore 无 decision 时持久化暂停，绑定 plan/request digest 的有效 decision 到达后恢复；
- memory governance：promotion/correction/forget 回查 ApprovalStore，并绑定 tenant/scope/record revision/content digest；旧批准不能跨 revision 重放，forget sink 传播可追踪；
- Operations：Harness、Approval、Memory 三 Store revision/digest join，display-safe redaction，SQLite snapshot replay/latest；
- Web HITL：要求 reviewer identity，动作写入 `AccountableApprovalExecutor`，返回 decision digest，审批状态与 release stage 同步；终态隐藏重复动作；
- 多前端同源：CLI/TUI/ImGui/Web 统一使用 `phase4_operations_bootstrap.hpp`，按 tenant/run 从同一 SQLite snapshot store 读取；Web 审批后的新 snapshot 在跨进程重启后仍可回放。

## 真实 UI 证据

- [Pending accountable approval](../../assets/ui/phase4-r3a-approval-pending.png)
- [Durable approved decision](../../assets/ui/phase4-r3a-approval-approved.png)
- [Shared persistent snapshot in TUI](../../assets/ui/phase4-r3a-shared-tui.png)
- [Shared persistent snapshot in ImGui](../../assets/ui/phase4-r3a-shared-imgui.png)

真实接口返回 `202`，decision digest 为 `sha256:0da4159fc70aea176b069464f710da1e05b9152c268d4fa871d6e0315ce9219c`。批准后截图显示 HITL `PASSED`、release stage `PASSED`、revision `r2`，按钮被移除；综合 architecture finding 仍为 warning，因此 overall 保持 warning，未被审批错误覆盖。

同源复核使用 `/tmp/taskflow-r3a-shared/operations.sqlite3`：Web 写入 `ops-demo-001.next`，decision digest 为 `sha256:844f77394b4f493b37e6d2b7f6a21e21e918a6b99a1c3c12675ee1e4fed9b444`；随后独立启动的 CLI、TUI、ImGui 均显示 `reviewer-r3a`、durable decision `r1`、release `r2/passed`。数据库路径仅是本次可丢弃运行夹具，仓库内截图是持久验收证据。

## 门禁

`phase4-offline` 51/51 PASS，`phase3-offline` 14/14 PASS，targeted 4/4 PASS，四个 demo build PASS，跨进程 CLI replay PASS，`git diff --check` PASS。SQLite operational API 统一经 `sqlite_api_boundary` 强制。

## 剩余边界

R3A 的 UI 同源边界已关闭；整体仍暂不标 complete：默认生产 Harness composition 尚未把 Cognition/Memory/Assurance/Remediation/Judge/Sandbox/Approval adapters 统一装配到非测试入口。当前 Web 证据是 deterministic demo runtime 上的真实持久化 approval action，不等于生产身份提供者认证。
