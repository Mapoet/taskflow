# WP4.4：Unified Sandbox Runtime

**优先级**：P0/P1  
**依赖**：WP4.2、WP4.3、WP4.9 memory connector trust boundary  
**下游**：所有 investigator/executor/verifier、WP4.7

## 1. 目标

把 Skill、filesystem、renderer 和不可信 Memory/RAG connector 的组件级隔离提升为统一任务执行环境，支持 process/container/remote provider，并产出可重放 workspace 和资源 manifest。项目可编辑文件、外部检索和历史工具输出默认是数据而非高权限指令。

## 2. 复用策略

以 Skill `unshare+bwrap` 实现为 ProcessSandbox 原型，抽取而非重写其 mount/network/env/secret/RLIMIT 逻辑；renderer 和不受信工具逐步迁移。FS jail 保留为工具内第二道边界。

## 3. 可执行任务

| ID | 任务 | 完成条件 |
|---|---|---|
| 4.4.1 | Sandbox types | spec/profile/capability/workspace/resource/network/credential/manifest |
| 4.4.2 | Provider interface | create/exec/upload/download/snapshot/diff/destroy/inspect |
| 4.4.3 | Process provider | bwrap/unshare、mount、network、env、RLIMIT、process tree cleanup |
| 4.4.4 | Workspace store | base digest、overlay、input/output diff、quota、retention |
| 4.4.5 | Network policy | deny default、DNS/IP/redirect/egress allowlist、audit |
| 4.4.6 | Credential broker | short-lived file/fd/env reference、scope、redaction、revocation |
| 4.4.7 | Container provider | image digest、rootless、seccomp/AppArmor/cgroup、read-only root |
| 4.4.8 | Remote provider | authenticated API、lease、heartbeat、upload integrity、cleanup |
| 4.4.9 | Skill migration | 现有合同测试在 provider 上等价通过 |
| 4.4.10 | Renderer/tool migration | 不受信 worker 统一走 sandbox；可信内建工具显式标注 |
| 4.4.11 | Policy integration | planner 声明 profile，PDP 校验，memory connector 最小权限，approval 绑定 spec/view digest |
| 4.4.12 | Escape/resource E2E | traversal、symlink、proc/dev、network、fork bomb、CPU/RAM/PID/I/O |

## 4. 五层验收

功能验证命令/文件/artifact；模块验证 policy/parser/lifecycle；集成验证 Skill/Tool/MCP/renderer；综合验证 workspace 可复现和宿主无污染；指标验证启动时延、执行开销、资源上限和回收成功率。

## 5. DoD 与回滚

不受信代码和 connector 不直接运行在 AgentServer 宿主；每次执行可由 image/base/input/policy/memory view digest 重放，并有 diff/资源/网络/credential 审计。Sandbox 输出只能产生 observation/candidate，不能自行写入高权威记忆。迁移按 tool class feature flag 进行；旧 Skill sandbox 在统一 provider 稳定前保留。
