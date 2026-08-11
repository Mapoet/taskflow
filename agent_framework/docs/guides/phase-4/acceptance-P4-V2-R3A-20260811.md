# P4-V2-R3A Approval / Memory / Operations 集成验收报告

**状态**：partial（integrated core accepted）  
**日期**：2026-08-11

## 已直接验证

- accountable approval：reviewer identity/role、SoD、delegation scope、高风险双人复核、vote CAS、SQLite restart reconciliation、edit 新 revision、expiry/stale/duplicate 拒绝；
- Harness：真实 ApprovalStore 无 decision 时持久化暂停，绑定 plan/request digest 的有效 decision 到达后恢复；
- memory governance：promotion/correction/forget 回查 ApprovalStore，并绑定 tenant/scope/record revision/content digest；旧批准不能跨 revision 重放，forget sink 传播可追踪；
- Operations：Harness、Approval、Memory 三 Store revision/digest join，display-safe redaction，SQLite snapshot replay/latest；
- Web HITL：要求 reviewer identity，动作写入 `AccountableApprovalExecutor`，返回 decision digest，审批状态与 release stage 同步；终态隐藏重复动作。

## 真实 UI 证据

- [Pending accountable approval](../../assets/ui/phase4-r3a-approval-pending.png)
- [Durable approved decision](../../assets/ui/phase4-r3a-approval-approved.png)

真实接口返回 `202`，decision digest 为 `sha256:0da4159fc70aea176b069464f710da1e05b9152c268d4fa871d6e0315ce9219c`。批准后截图显示 HITL `PASSED`、release stage `PASSED`、revision `r2`，按钮被移除；综合 architecture finding 仍为 warning，因此 overall 保持 warning，未被审批错误覆盖。

## 门禁

`phase4-offline` 51/51 PASS，`phase3-offline` 14/14 PASS，targeted 4/4 PASS，`git diff --check` PASS。SQLite operational API 统一经 `sqlite_api_boundary` 强制。

## 剩余边界

R3A 暂不标 complete：需要让默认生产 Harness composition 使用这些 adapters，并补 CLI/TUI/ImGui 从同一持久化 snapshot 读取的运行证据；当前 Web 证据是 deterministic demo runtime 上的真实持久化 approval action，不等于生产身份提供者认证。
