# Portable Tools & Skills Closure（AT0→AT9）

> 状态：AT0 基线已冻结；实现与认证按 AT1→AT9 推进。
> 依据：`af-target-v2.md`、`tools/claude.md`、`tools/af-tools.md`。

## 1. 目标与边界

AF 对外只提供一套可供模型和 Skill 使用的工具契约。Claude Code、Cursor 与
Codex 风格 Skill 可直接请求常见工具名称，不再要求维护一份 AF 专用
`fs_*`/`web_*` 清单。内部既有实现可以复用，但不能继续作为平行的 LLM 工具面。

“兼容”指 AF portable profile 对常见 Skill 工具名称、核心输入语义、权限和生命周期
提供稳定适配；不声称三个外部产品存在共同的正式 ABI。

## 2. `agent.portable_tool_profile/v1`

| 公共名称 | canonical capability | 旧入口（仅迁移） | 副作用 |
|---|---|---|---|
| `Read` | `filesystem.read/v1` | `fs_read` | read |
| `Write` | `filesystem.write/v1` | `fs_write` | write |
| `Edit` | `filesystem.edit/v1` | `fs_replace` | write |
| `Glob` | `filesystem.glob/v1` | `fs_search` | read |
| `Grep` | `filesystem.grep/v1` | `fs_grep` | read |
| `Bash` | `process.shell/v1` | — | unknown/write |
| `Python` | `process.python3/v1` | — | unknown/write |
| `CMake` | `process.cmake/v1` | — | unknown/write |
| `Make` | `process.make/v1` | — | unknown/write |
| `Curl` | `network.curl/v1` | — | unknown |
| `Wget` | `network.download/v1` | — | write |
| `Sed` | `filesystem.edit/v1` | — | write |
| `LS` | `filesystem.glob/v1` | `fs_list_dir` | read |
| `Cat` | `filesystem.read/v1` | — | read |
| `WebFetch` | `network.web-fetch/v1` | `web_fetch` | read |
| `WebSearch` | `network.web-search/v1` | `web_search` | read |
| `Skill` | `skill.load/v1` | implicit injection | read |
| `Calculate` | `compute.expression/v1` | `expr_eval` | read |
| `ValidateExpression` | `compute.expression-validate/v1` | `expr_validate` | read |
| `BatchCalculate` | `compute.expression-batch/v1` | `expr_batch_eval` | read |
| `RenderChart` | `visual.chart-render/v1` | `draw_render` | read |
| `ExportChart` | `visual.chart-export/v1` | `draw_export` | write |

旧入口不得出现在 `export_as_llm_tools()`。迁移期调用先解析到 canonical identity，所有
allowlist、Skill grant、hook、schema、审计和 receipt 均使用同一身份；原始名称只用于诊断。

## 3. 安全不变量

1. alias 不能扩大权限；未知、冲突或循环 alias fail closed。
2. 参数被 hook 修改后必须重新授权并重新校验 schema。
3. 写操作采用 workspace jail、原子提交和 revision/digest 并发保护。
4. 进程工具使用参数数组、固定 executable、最小环境、资源限制和可取消沙箱。
5. 网络默认拒绝，按域名/策略授权；重定向逐跳重新检查 SSRF 与权限。
6. background 工具必须提供 durable attach/cancel/reconcile，`attach` 不得退化为 `run`。
7. 节点权限为 template、invocation、pinned Skill、node request、runtime policy 的交集。

## 4. 连续实施与证据门禁

- **AT1**：canonical identity、隐藏迁移 alias、权限/allowlist/hook/审计归一化。
- **AT2**：v0/v1 `allowed-tools`、多 Skill 根、冲突诊断和 revision pinning。
- **AT3**：文件工具完整契约及 traversal/symlink/TOCTOU/并发测试。
- **AT4**：Bash/Python/CMake/Make 进程工具，Curl/Wget 网络工具，Sed/LS/Cat/Grep
  文件兼容工具，以及 Bubblewrap、取消/超时/background/restart。
- **AT5**：Web、Skill、求值、绘图公共工具与资源渐进披露。
- **AT6**：AgentTemplate production runner 和节点最小权限。
- **AT7**：统一事件回放、LLM observability、实时 observation snapshot。
- **AT8**：production builder、五个正式 demo、旧工具面清理和迁移诊断。
- **AT9**：契约、功能、安全、生命周期、跨 Skill、AgentTemplate、回归和 UI 实机认证。

每阶段必须有直接覆盖该阶段不变量的自动化证据；窄单测不能代替综合完成声明。
