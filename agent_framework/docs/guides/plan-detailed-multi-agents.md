# WP2.agents：多 Agent 协同工作模式 — 实施计划

**文档版本**：1.0  
**日期**：2026-04-05  
**性质**：在 [plan-detailed.md](./plan-detailed.md) 阶段 2、[plan-detailed.v2.md](./plan-detailed.v2.md)（WP2.0 / WP2.1b–2.1d / §5.1 / §9）、[phase-2-plan.md](./phase-2-plan.md)（含 **WP2.agent2agent**）之上的 **追加工作包** 详案。  
**关联**：`phase-2-wp-agent2agent.md`、`a2a-orchestrator.md`、`tool-orchestration.md`、`context-budget.md`、`a2a-spec-tracker.md`。

---

## 1. 目标陈述（已拍板，无疑点）

| 维度 | 要求 |
|------|------|
| **并发** | 单次用户请求内 **多个子任务真并行**（对不同 peer 或同 peer 多 task_id）；编排逻辑可 **穿插**（submit 后不立刻 wait）；仍支持 **顺序 await**（显式 `wait` 工具）。 |
| **非阻塞语义** | 允许在 **本编排会话** 中：上一批远程任务未全部完成时，处理 **新的用户输入** 或 **启动别的子图**；支持 **停止** 已提交任务（`cancel` + 会话级策略）；**当前交付 CLI 仍为单会话 REPL**（不强制多租户 UI）。 |
| **可观测性（三者都要）** | **(A)** 终端/结构化日志；**(B)** 可选 **注入 LLM 上下文**（系统/用户槽位或专用 `Message`，见 §6）；**(C)** **可查询** 的近期子任务事件与快照（工具或会话状态 API）。 |
| **工具面** | 细粒度：**submit**、**get_status**、**wait**、**cancel**；另需 **延长超时**；保留 **`run_remote_task_and_wait`** 作为简易模式（SendMessage + 同步等到终态）。 |
| **可靠性** | **Cancel** 与 **超时** 必选；超时后可 **延长**（`extend`）；**多任务部分失败** → **按 task 分别** 报告，不概化为单次 boolean。 |
| **会话索引** | **Peer 维** 索引足够：`peer_id` → `context_id`（延续现有 `PeerSessionBook`）；全局 **`task_id`** 表 + **`peer_id`** 外键。 |

---

## 2. 非目标（本工作包不宣称交付）

- **多用户多会话服务端编排**（多 `tenant` 隔离、队列公平性）— 仅预留 `session_id` 字段与文档，与 [plan-detailed.v2.md](./plan-detailed.v2.md) §5 / §9 对齐。  
- **跨进程恢复** 未完成远程任务（WP3.6 落盘后再接）。  
- **DAG 硬依赖调度器**（任务 B 必须等任务 A 的图节点）；首版用 **LLM + 工具** 表达依赖即可。  
- **修改 Google A2A 规范本身**；仅 **实现客户端行为** 与内部状态机。

---

## 3. 与 WP2.agent2agent 的关系

| 现状 | 本计划 |
|------|--------|
| `register_a2a_orchestrator_tools` + `a2a_send_message`（或等价名）同步 `run_remote_task_and_wait` | **保留** 该路径；新增 **细粒度工具集** 与 **OutboundTaskSupervisor**（见 §4）。 |
| `ToolSideEffect::Write` → WP2.1b **写串行** | 细粒度工具拆开：**ReadOnly** = `get_status`、**只读列举**；**Write** = `submit`、`wait`、`cancel`、`extend`；并对 **`submit` 并行** 做 **显式策略**（§5.2），避免与「写串行」定义冲突。 |
| `PeerSessionBook` | **保留**；监督器在 terminal 时 **写回** `context_id`。 |

**工作包命名**：**WP2.agents**（可与总表 **WP2.agent2agent** 并列；实现 PR 可先落库代码，再在 `phase-2-plan.md` 增加一行交叉引用）。

---

## 4. 架构组件（必须实现的 C++ 模块）

### 4.1 `OutboundTaskSupervisor`（进程内、单编排会话默认可单例）

**职责**：

- 维护 **`TrackedTask`**：`local_handle`（UUID 或单调 id）、`peer_id`、`remote_task_id`、`phase`（`submitted` / `monitoring` / `terminal`）、`status`（与 `AgentTaskStatus` 对齐）、`last_error`、`deadline`（`steady_clock` 或 wall + duration）、`summary_cache`、`context_id_snapshot`。  
- **`submit`**：`SendMessage`（复用 `AgentClient`）→ 立即返回 **handle + remote_task_id**；启动 **非阻塞** 监测回路（见 §4.2）。  
- **`cancel`**：调用远端 **CancelTask**（若规范/Server 支持）；本地 **无论如何** 将任务标为 `cancelled` / `failed` 并停监测。  
- **`extend`**：仅 **未完成** 任务；**延长本地 deadline**；可选二次调用无操作（文档写明）。  
- **并发**：内部 `mutex`；监测使用 **每任务独立** `std::thread` 或使用 **共享 `asio`/timer 泵**（首版推荐 **每任务一线程 + `jthread` 可打断** 或条件变量，与控制块耦合清晰；压测后再收敛）。

**头/源建议路径**：`include/agent/a2a/outbound_task_supervisor.hpp`、`src/a2a/outbound_task_supervisor.cpp`。

### 4.2 监测回路（SSE 优先，GetTask 轮询兜底）

- 与现有 `run_remote_task_and_wait` **共用** `AgentClient::subscribe_task_updates` / `get_task` 逻辑，抽成 **`monitor_peer_task(peer_id, task_id, callbacks)`**，避免三处复制。  
- **事件回调**（写入监督器并 **广播** 到 §6 三通道）：`on_status`、`on_artifact`（可选）、`on_deadline_local`（本地超时，不等同远端 failed）。

### 4.3 会话级策略 `OutboundSessionPolicy`（结构体 + 默认）

| 字段 | 语义 |
|------|------|
| `cancel_all_on_new_user_turn` | 默认 **`false`**（避免误杀）；CLI 可用 env **`AGENT_A2A_CANCEL_ON_NEW_TURN=1`** 开启。 |
| `max_parallel_submits_per_iteration` | 默认与 `AgentConfig::max_parallel_read_tools` 分离，建议新字段 **`max_parallel_a2a_submits`**，默认 `4`，`<=0` 视为 `1`。 |
| `default_timeout_ms` | 继承 `peers.json` / registry；`extend` 增量单位 ms。 |

**新用户轮次钩子**：在 `GraphExecutor` / CLI REPL **合入用户消息前**（与 WP2.0 状态写回同层），若策略为真 → `supervisor.cancel_all_active()`（或仅 `cancel` 未 `wait` 的任务，**文档固定一种**，建议：**仅 cancel 仍处于 `submitted`/`working` 且未被当前 `wait` 集合声明依赖的任务** — 实现简单版：**cancel all active** 作为 v1）。

---

## 5. ToolBus 工具表（名称、Schema、副作用）

**统一前缀**：`a2a_`（与现有一致）。下列 **ToolMeta** 必须注册；**schema** 为 JSON Schema 草案，与现 `register_local_tool` 一致。

### 5.1 `a2a_submit_task`（Write）

**参数**（最小）：

- `peer_id`（string，必填）  
- `user_text`（string，必填）  
- `continue_session`（bool，默认 `true`）  
- `metadata`（object，可选）  
- `timeout_ms`（int，可选，覆盖 peer 默认）  
- `monitor`（bool，默认 `true`）— `false` 时仅 SendMessage 返回，不启后台监测（**高级**，默认不开）

**返回**（成功）：

```json
{
  "ok": true,
  "local_handle": "…",
  "peer_id": "…",
  "remote_task_id": "…",
  "context_id": "…或null",
  "deadline_ms_from_now": 123000
}
```

### 5.2 `a2a_get_task_status`（ReadOnly）

**参数**：`local_handle` **或** (`peer_id` + `remote_task_id`)  

**返回**：`phase`、`status`、`summary_text`（短）、`error`（若有）、`deadline_remaining_ms`。

→ **WP2.1b**：可与其它只读工具 **并行**（同迭代多条 `get_status`）。

### 5.3 `a2a_wait_tasks`（Write）

**参数**：

- `handles`（string 数组）或 `peer_task_pairs`（array of `{peer_id, remote_task_id}`）  
- `timeout_ms`（可选，**本次 wait 上限**，与单任务 deadline 取 **min**）  
- `mode`：`all` \| `any`（默认 `all`；`any` = 任一 terminal 即返回，余下仍在跑）

**返回**：

```json
{
  "ok": true,
  "results": [
    {"local_handle": "…", "peer_id": "…", "remote_task_id": "…", "ok": true/false, "status": "completed", "summary_text": "…", "error": null}
  ],
  "partial": true
}
```

**语义**：`partial: true` 当 **mode=all** 且 **超时** 或 **部分 failed/cancelled**；每条 **独立** `ok`。

### 5.4 `a2a_cancel_task`（Write）

**参数**：`local_handle` 或 (`peer_id` + `remote_task_id`)  

**返回**：`ok`、`prev_status`、`remote_ack`（bool，远端是否确认）。

### 5.5 `a2a_extend_task_timeout`（Write）

**参数**：`local_handle`、`extra_ms`（int > 0）  

**返回**：`ok`、`new_deadline_ms_from_now`。

### 5.6 `a2a_list_subtasks`（ReadOnly）

**参数**：`since_seq`（int，可选，事件序号游标）、`peer_id`（可选过滤）、`limit`（默认 20）  

**返回**：**快照列表** + `next_seq`（供查询）。

### 5.7 简易模式（保留）

- **`run_remote_task_and_wait`**：**不删**；内部可 **重构为** `submit(sync=true)` + `wait` 单条，或保持独立代码路径但 **共享** `monitor_*` 辅助函数。  
- ToolBus 名：**沿用现有** `a2a_send_message`（或文档中登记的历史名），其 `description` 标明 **sync / convenience**。

### 5.8 WP2.1b 并行策略修正项（本计划硬性规定）

- **`a2a_submit_task`**：在 **单次 LLM 迭代** 内，若模型下发 **多条** `a2a_submit_task`，执行器按 **`max_parallel_a2a_submits`** **并行** `call_tool`（或监督器内并行提交），**区别于** 一般 Write 串行。  
- **`a2a_wait_tasks` / `a2a_cancel_*` / `a2a_extend_*`**：**串行** 或与提交分离（实现文档写在 `tool-orchestration.md` **附录 WP2.agents**）。  
- **`a2a_get_task_status` / `a2a_list_subtasks`**：**ReadOnly**，享 **WP2.1b** 并行读。

**环境变量**（写入 `envs_status.md`）：

- `AGENT_A2A_MAX_PARALLEL_SUBMITS`（可选覆盖配置）  
- `AGENT_A2A_CANCEL_ON_NEW_TURN`（0/1）  
- `AGENT_A2A_SUBTASK_LOG`（`stderr` \| `none` \| 未来 `jsonl` 路径）

---

## 6. 可观测性三通道（验收标准）

### 6.1 通道 A — 日志 / 终端

- 每条状态迁移打一条 **JSON 行** 或前缀行：`[a2a_subtask]` + `component=subtask_event` + 字段与 [plan-detailed.v2.md](./plan-detailed.v2.md) §9 对齐（`peer_id`、`task_id`、`local_handle`、`status`、`outcome`）。  
- **与** 现有 `on_remote_log` **合并策略**：`merge_streams` 时仍可将 **摘要行** 送进 `stream_callback`（demo 层）。

### 6.2 通道 B — LLM 上下文注入

- **Hook 点**：在 **`PromptRenderer`** 拼装 `LLMInput` **之前**，增加可选步骤 **`append_subtask_digest(state)`**（由 `AgentThreadState` 持 **weak_ptr** 到 supervisor 或持 **`SubtaskDigest` 缓存**）。  
- **内容**：最近 N 条事件（可配 **`AGENT_A2A_SUBTASK_CONTEXT_MAX_EVENTS`**，默认 8）+ **未完成句柄列表**（handle + peer + age_ms）。  
- **预算**：计入 **WP2.1c**（与工具结果同一 meters 或独立 `subtask_context_bytes` 帽）；超限 **截断** 并 `_af_truncated`。  
- **触发**：每轮 LLM 前刷新；**不**阻塞用户打字（仅拼装时读取）。

### 6.3 通道 C — 可查询

- 工具 **`a2a_list_subtasks`** **+**（可选）`AgentThreadState::debug_subtasks_json()` 供测试断言。  
- **事件环缓冲**：环形队列，默认 **256** 条，可配置。

---

## 7. 与 GraphExecutor / AgentLoop 的衔接

| 接入点 | 工作 |
|--------|------|
| **状态对象** | `internal::AgentThreadState` 增加 `std::shared_ptr<a2a::OutboundTaskSupervisor> outbound_tasks`（或工厂按 `session` 创建）；**生命周期** = 单次 CLI 进程或 REPL 会话（与 WP2.0 `history` 同期）。 |
| **工具注册** | `register_a2a_orchestrator_tools` **扩展** 重载：接受 `OutboundTaskSupervisor&`；旧签名 **委托** 新建默认监督器（仅 sync 工具路径）或 **静态**监督器（文档 **deprecated** 多会话风险）。 |
| **新用户轮次** | REPL 读取下一行 → 若 `AGENT_A2A_CANCEL_ON_NEW_TURN=1` → `supervisor->user_turn_barrier()`（命名示例）执行 §4.3。 |
| **Graph 多子图** | 同一进程多 `GraphExecutor::execute`：**每会话一个 supervisor**；禁止全局单例跨会话（除非 tests）。 |

---

## 8. 任务分解与 PR 顺序（可操作）

| PR | 内容 | DoD |
|----|------|-----|
| **PR-1** | 抽出 **`monitor_peer_task`internal`**；`run_remote_task_and_wait` **改为调用** 该核心（行为不变）。 | 现有 `test_a2a_orchestrator_tools`、契约测 **全绿**；无 API 破坏性。 |
| **PR-2** | 实现 **`OutboundTaskSupervisor`**（submit / 后台监测 / get_snapshot / cancel / extend / 事件环）。 | **单元测** `test_a2a_outbound_supervisor`：假 `AgentClient` 注入；测并发 submit=3、超时、cancel、extend。 |
| **PR-3** | 注册 §5 工具；**ReadOnly** 分类；`a2a_submit` **并行** 路径接 `AgentLoopNode`（或 `execute_tool_calls_sequenced` 扩展 **A2A submit 特例**）。 | **单测** `test_a2a_multitool_orchestration`：同轮 2×submit → get_status → wait all。 |
| **PR-4** | **PromptRenderer** 注入 digest + **WP2.1c** 计量；`a2a_list_subtasks`。 | 单测：digest 字节帽；`list` 游标。 |
| **PR-5** | **`cli_a2a_orchestrator_demo`**：flags 打开/关闭 digest、并行度；**system_prompt** 更新（工具说明）。 | 手动脚本 + 可选 **live** 测（已有 pattern）。 |
| **PR-6** | 文档：`a2a-orchestrator.md`、`tool-orchestration.md` 附录、`envs_status.md`；**`phase-2-plan.md`** 增加 **WP2.agents** 行与此文链接。 | Review checklist 勾选 |

---

## 9. 测试矩阵（最低）

| ID | 场景 |
|----|------|
| T-1 | 双 peer loopback：并行 submit → 双 completed → `wait mode=all` |
| T-2 | 一成功一失败：`results[]` 各自 `ok`，`partial=true` |
| T-3 | 本地 deadline → `extend` → 完成 |
| T-4 | `cancel` 后 `get_status` = `cancelled` 或 failed，监测线程退出 |
| T-5 | `AGENT_A2A_CANCEL_ON_NEW_TURN=1`：第二行用户输入 → active 任务被取消（断言 RPC mock 收到 Cancel） |
| T-6 | **WP2.1b**：同轮 3×`get_status` 并行（mock 延迟）总墙钟 < 串行和 |
| T-7 | **WP2.1c**：digest 注入超长 → 截断字段出现 |

---

## 10. 风险与缓解

| 风险 | 缓解 |
|------|------|
| 线程数爆炸（每任务一线程） | `max_parallel_a2a_submits` + supervisor 内部 **线程池**（PR-2 后记技术债，首版可限制 **同时监测任务 ≤32**） |
| 与 WP2.1b 「写串行」文字冲突 | `tool-orchestration.md` **明文**：A2A submit 为例外 |
| LLM 滥用 wait | system_prompt **引导**先 submit 多再单次 wait；工具 **description** 写明成本 |
| Cancel 远端未实现 | **本地**仍停止监测；`remote_ack=false` **显式返回** |

---

## 11. 修订记录

| 日期 | 版本 | 说明 |
|------|------|------|
| 2026-04-05 | 1.0 | 初稿：WP2.agents 详案（并发 submit、wait/cancel/extend、三通道可观测、PR/测试矩阵）；与 plan-detailed / v2 / phase-2 对齐 |
| 2026-04-05 | 1.1 | 实现落库：`a2a_task_monitor` 共享监测；`OutboundTaskSupervisor`；细粒度工具注册；`execute_tool_calls_sequenced` 连续 `a2a_submit_task` 并行批；`LLMInput::orchestrator_subtask_digest` + `PromptRenderer`；`AgentThreadState::outbound_supervisor`；`cli_a2a_orchestrator_demo` 接线；CTest `a2a_outbound_supervisor` / `a2a_multitool_orchestration`；指南 [a2a-orchestrator.md](./a2a-orchestrator.md)、[tool-orchestration.md](./tool-orchestration.md) 附录、[envs_status.md](./envs_status.md) §3.13。 |

### 11.1 实现状态（库内）

- [x] PR-1 监测核心复用（`monitor_remote_task_until_deadline`）  
- [x] PR-2 `OutboundTaskSupervisor` + `test_a2a_outbound_supervisor`  
- [x] PR-3 细粒度工具 + 并行 submit + `test_a2a_multitool_orchestration`  
- [x] PR-4 digest / `list_subtasks` / 线程状态持有 supervisor  
- [x] PR-5 `cli_a2a_orchestrator_demo`（supervisor、`on_user_turn_barrier`、细粒度注册）  
- [x] PR-6 文档与 env 表  

---

## 12. 相关链接

- [plan-detailed.md](./plan-detailed.md)  
- [plan-detailed.v2.md](./plan-detailed.v2.md)  
- [phase-2-plan.md](./phase-2-plan.md)  
- [phase-2-wp-agent2agent.md](./phase-2-wp-agent2agent.md)  
- [a2a-orchestrator.md](./a2a-orchestrator.md)  
- [tool-orchestration.md](./tool-orchestration.md)  
- [context-budget.md](./context-budget.md)

