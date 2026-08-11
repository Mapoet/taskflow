# P4-V2-R2X Artifact Execution 增量验收报告

**状态**：partial（filesystem closure accepted）  
**日期**：2026-08-11

新增受工作区 jail 约束的真实产物执行器：仅执行已批准 action，使用稳定 idempotency key 返回可重放 receipt，原子写入后重扫真实文件并生成 content、workspace diff 和 manifest digest；rollback 恢复 preimage 或删除新增文件。

独立 filesystem oracle 重新读取文件并与 manifest/requirement 比较。系统用例刻意遗漏 README，首次验证产生 finding；最小修复生成新 manifest digest，旧 evidence 不可复用，强制复验后才通过。路径穿越、符号链接写穿、未批准 action、事后篡改均被拒绝。

证据：`phase4_artifact_execution` 1/1 PASS、`phase4-harness` 4/4 PASS、`phase4-offline` 47/47 PASS、`phase3-offline` 14/14 PASS、`git diff --check` PASS。

SQLite durable artifact journal 已验证跨进程 replay/reconciliation；R1I Harness adapter 已验证真实首次拒绝、最小修复、父子 manifest、选择性复验、最终完成门禁，以及失败修复达到循环上限后进入 ManualReview。

尚未关闭：由具体 SandboxProvider 支撑的 build/test/runtime/security/metric command oracle；它按计划在 R4O 与 Process Sandbox 一并关闭。因此 filesystem artifact closure 和 Harness repair loop accepted，RC-02 在 R4O 前仍保持 partial。
