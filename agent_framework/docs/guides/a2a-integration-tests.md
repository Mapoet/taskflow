# WP2.a2a-test：A2A 完整集成 Online Live 测试 — 实施计划

**路径**：`agent_framework/docs/guides/a2a-integration-tests.md`  
**版本**：0.2  
**日期**：2026-04-05  
**状态**：实施计划（与代码里程碑对齐，随 WP 完成度滚动更新）

---

## 1. 文档目的与范围

### 1.1 目的

在 **WP2.0 / WP2.1（含 WP2.1a）/ WP2.2 / WP2.3 / WP2.4** 等前置能力按 DoD 落地后，定义 **分层、可重复、门禁清晰** 的 **Agent-to-Agent（A2A）完整集成测试**：从 **无网契约** 到 **本机 HTTP 回环**，再到 **可选 online live**（真实 listen、真实网络拓扑、多 Agent 实例）。

### 1.2 范围（本计划覆盖）

| 覆盖 | 说明 |
|------|------|
| 测试分层与验收标准 | Tier A–E，每层的 **前置条件、步骤、通过判据、产物** |
| 与上游 WP 的依赖矩阵 | 哪一层测试 **硬依赖** 哪几个 WP；哪些为 **增强项** |
| 环境与 CI 策略 | 默认无网；live 用 **显式环境变量** 闸门 |
| 多 Agent 协同 | **测试画像**（Card、`skills`、内部 ToolBus/MCP 子集），**不**要求先于 WP2.4 完成全量 MCP 目录 |
| 与现有文档关系 | 规范锚点：[a2a-spec-tracker.md](./a2a-spec-tracker.md)；契约快照：[phase-2-wp6.md](./phase-2-wp6.md)；阶段总览：[phase-2-plan.md](./phase-2-plan.md) |

### 1.3 不在本文件定义的内容

- **具体 JSON-RPC method 字符串**（以 WP2.1 填满后的 **`a2a-spec-tracker.md` §3** 为唯一权威；本计划只写「调用规范列出的任务创建 / 查询 / 取消 / … 方法」）。
- **OAuth / 复杂企业 SSO** 的 live 矩阵（归 **WP2.5**；本计划在 Tier 中标注「有 WP2.5 时追加用例」）。
- **动态 MCP 热加载**（阶段 3 **WP3.7**，阶段 2 不纳入 DoD）。

---

## 2. 命名与里程碑对应

| 名称 | 含义 |
|------|------|
| **WP2.a2a-test** | 本文件描述的 **测试实施工作包**（可拆 PR：先 Tier A+B 入 CI，再 Tier C+D 可选目标，最后 Tier E 与运维文档） |
| **M2**（见 [phase-2-plan.md](./phase-2-plan.md) §8） | WP2.1a + WP2.1 + WP2.2 + WP2.3 **打通 happy path + SSE** — **Tier B 最低门槛** |
| **M3** | WP2.4 + WP2.5 + WP2.6 **契约门禁** — **Tier A 与 WP2.6 对齐**；**Tier C** 在 WP2.4 后具备 Client 规范侧 |

---

## 3. 前置依赖矩阵（硬 / 软）

### 3.1 按测试 Tier 的依赖

| Tier | 硬依赖（未满足则本 Tier 不测） | 软依赖（增强覆盖，可后续补） |
|------|-------------------------------|------------------------------|
| **A** 契约与 fixture | WP2.1a（`jsonrpc`、`wire_card`、`a2a-spec-tracker.md` Card/§5）；WP2.1（§3 方法表、Task/Message/Artifact wire、SSE schema）；**WP2.6** 落地后与 **manifest + 黄金 JSON** 对齐 | WP2.4（Client 解析 fixture 响应的 **同一 Facade**） |
| **B** 本机回环 HTTP | **WP2.0**（`GraphExecutor::execute` + 会话写回，Server 与 CLI 同路径）；**WP2.2**（真实路由、**listen 线程不阻塞** executor）；**WP2.3**（任务状态机、取消/超时映射到 wire）；WP2.1 业务 JSON-RPC + SSE | WP2.1b–d（并行工具、预算、Hook）；WP2.7（输入质控 `-32602` 路径） |
| **C** Online live 单 Agent | Tier B 全过；**WP2.4**（`AgentClient` 默认走规范 JSON-RPC + `agent_card_from_a2a_wire`）；固定 **base URL** 与 **Well-Known** 行为与 tracker §2 一致 | WP2.5（鉴权头）；WP2.1c（大 payload / Artifact 外置） |
| **D** 多 Agent 协同 | Tier C 单 Agent 过；**至少两个** 可区分 **base URL** 的 Agent 进程（或容器）；各进程 Card 中 **`skills` 与 `name` 可区分** | WP2.8（Verifier 作为「审核 Agent」内部能力时）；WP2.1b（审核侧只读并行） |
| **E** 非功能与安全冒烟 | Tier C 过；运维提供 TLS/防火墙策略（若 live 非 localhost） | 压测工具、WAF 等（可选） |

### 3.2 WP2.0 对测试的含义（可操作）

- **同一套图模板**：Live 与 CLI 使用同一 `GraphExecutor` 入口，避免「Server 特供 stub 图」导致 live 无效。
- **会话延续**：对「同一 `session_id` / 任务上下文」的 **第二轮** JSON-RPC 调用，`history` 或等价状态 **可见延续**（与 phase-2-plan **D11** 一致）；测试用例中 **显式断言** 第二轮与第一轮差异（例如消息条数增加）。

### 3.3 WP2.3 对测试的含义（可操作）

- 每个任务测例 **记录**：创建时的 **官方 wire 状态**、SSE 或轮询观察到的 **迁移序列**、终态 **completed / failed / canceled**（字符串以 tracker WP2.3 更新为准）。
- **取消**：调用规范定义的 **取消 method** 后，在 **超时时间 T_cancel** 内观察到 **取消成功语义**（无忙等死锁；listen 线程仍响应）。
- **超时**：配置 **短超时**（如 5–15s）的图或 mock 慢节点，断言 **FAILED** 与 **可序列化原因** 字段存在。

---

## 4. 测试分层（Tier A–E）— 实施规格

以下每一层均包含：**目标**、**环境**、**步骤**、**通过判据**、**失败时收集项**、**推荐 CTest/可执行文件名**（实现阶段按 CMake 实际命名为准）。

### 4.1 Tier A — 契约、编解码与黄金 fixture（默认 CI、无网）

**目标**：不启动 TCP listen，验证 **线协议与 C++ 模型** 与 **`a2a-spec-tracker.md`** 一致；与 **WP2.6** `tests/fixtures/a2a/<bundle_id>/` 及 `manifest` 同源。

**环境**：

- 无 `HTTP(S)` 外连。
- 已存在：`AGENT_TEST_A2A_FIXTURE_DIR`、`AGENT_TEST_A2A_TRACKER_PATH`（见当前 `CMakeLists.txt` 对 `test_a2a_*` 的配置）。

**步骤（须实现为稳定单测或可执行文件）**：

1. **JSON-RPC 通用层**（WP2.1a）：非法 batch、notification、`params` 为 array、缺 `id` 等 → 与 **tracker §8** 错误码一致（与现有 `test_a2a_jsonrpc` 同族扩展）。
2. **Card round-trip**：`card_min.json` 及 **至少一份「含非空 `skills`」** fixture → `from_a2a_wire` → `to_a2a_wire` → **语义字段**一致（允许 tracker 已声明的 **非保留字段丢失**，与 WP2.1a 未知键策略一致）；**必填键缺失** → `std::invalid_argument` 或项目统一错误类型。
3. **任务 wire**（WP2.1）：对每个已支持的 **Task/Message/Artifact** JSON 样例：**parse → 内部类型 → serialize** 与黄金文件 `equals_after_parse`（**WP2.6** 流程：`AGENT_A2A_UPDATE_GOLDENS=1` 仅人工门禁更新）。
4. **SSE 事件**（WP2.1）：对每种注册的事件名，**单行 `data:` JSON** 与 **多帧** 样例解析成功，且 **未知事件** 策略与实现文档一致（丢弃 / 记录 / 严格失败 — **须在 tracker 或 wp2 文档锁定一种**）。

**通过判据**：

- 上述用例 **0 失败**；**不**依赖机器 hostname 与时区（JSON 比较使用固定 `id` 与规范化时间戳或 mock clock）。
- **tracker §3** 中每行 method，在 **dispatch 表** 中有对应单元测 **或** WP2.6 bundle 中 **至少一条** 请求/响应对。

**失败时收集项**：失败用例名、**请求/响应 JSON diff**、**tracker 版本号 / git sha**（manifest 中记录）。

**推荐目标名**：`a2a_contract_json`、`a2a_contract_sse`（与 [phase-2-wp6.md](./phase-2-wp6.md) 一致）；扩展现有 `test_a2a_wire_card`、`test_a2a_jsonrpc`。

---

### 4.2 Tier B — 本机回环：Server + Client（默认 CI 可选或专用 job）

**目标**：**单进程或双进程本机**，验证 **GET Well-Known** + **POST Card.url** + **任务生命周期** + **SSE（若启用）**；验证 **WP2.2 线程模型**（listen 不阻塞长任务）。

**环境**：

- `127.0.0.1` 与 **动态分配端口**（避免硬编码 8080）；或 CMake 传入 `TEST_PORT`。
- 环境变量（建议，实现时与 `agent_server` 文档对齐）：
  - `AGENT_A2A_INTEGRATION_LOOPBACK=1`：**启用** Tier B 测试目标（未设置时 `SKIP`）。
  - `AGENT_A2A_STRICT=1`（与 [phase-2-wp2.md](./phase-2-wp2.md)）：仅注册 tracker 规定 JSON-RPC 路径。

**步骤**：

1. 启动 **httplib Server**（WP2.2），注册：
   - `GET /.well-known/agent-card.json` → 返回与 **运行时 base URL** 一致的 **`url` 字段**（禁止 Card 写死错误端口）。
2. **Client**（WP2.4）：`discover_agent` → **`agent_card_from_a2a_wire`** → 得到 `api_endpoint`（或等价）→ 后续 **仅** 向该 URL POST JSON-RPC。
3. 调用 **任务创建 method**（§3 为准）→ 获得 **task id**。
4. **分支 4a**：若 `capabilities` 含 **streaming**：打开 **SSE** 连接（路径与 WP2.2 文档一致），在 **T_sse** 内收到 **至少一条** 与任务相关事件，且终态与 **WP2.3** 一致。  
   **分支 4b**：若无 SSE：轮询 **任务查询 method**，直到终态或 **T_poll** 超时。
5. **并发探针**：在同一 Server 上 **并行** 发起 **N=8~32** 个「短任务」（echo 或立即完成），**无死锁**、无 **listen 线程** 卡死（可用超时 watchdog 进程外检测）。

**通过判据**：

- 步骤 1–4 **100%** 成功；**Well-Known 的 `url` 与 POST 目标一致**（自动化断言）。
- 并发探针 **全部**在 **T_multi**（建议 ≤ 60s）内完成。

**失败时收集项**：Server 日志、Client 日志、**最后一个 JSON-RPC id**、**active_tasks** 快照（若实现暴露调试接口则记录，**不**对公网开启）。

**推荐目标名**：`a2a_loopback_happy`、`a2a_loopback_concurrent`（或合并为一个可执行文件内多用例）。

---

### 4.3 Tier C — Online live：单 Agent、真实网络语义

**目标**：在 **可控主机**（本机、CI runner、或内网 VM）上运行 **与产物一致** 的 Server 二进制；Client 可为 **同仓库测试驱动** 或 **curl + jq** 脚本（须在 `tests/scripts/` 或文档中 **给出完整命令**，可复制执行）。

**环境**：

- **闸门**：`AGENT_A2A_LIVE_TEST=1` **且** `AGENT_A2A_LIVE_BASE_URL=https://host:port`（或 `http://` 仅限 dev）；未设置则 **整 Tier SKIP**。
- **WP2.5**：若 Server 要求 `Authorization` / API Key，则增加 `AGENT_A2A_LIVE_TOKEN` 或 header 文件路径，**文档写明**，**禁止**把密钥写入仓库。

**步骤**：

1. 运维或脚本启动 Server，健康检查：`GET /.well-known/agent-card.json` → **HTTP 200**，`Content-Type` 为 JSON。
2. 校验 **TLS**（若 `https`）：证书主机名匹配或 CI 使用 **受信测试 CA**（文档说明 `SSL_CERT_FILE`）。
3. 跑 **与 Tier B 同序** 的发现 → 创建任务 → SSE 或轮询至完成。
4. **负例**（须各至少 1 条）：
   - 非法 JSON body → **Parse error** 行为与 tracker **§8** 一致；
   - 未知 `method` → `-32601`；
   - 畸形 `params`（缺必填键）→ `-32602`；
   - **WP2.7**：若启用严格输入策略，**恶意 `@file` 路径** → `-32602` 且 `message` 前缀 **`input_policy_violation`**（与 [phase-2-wp7.md](./phase-2-wp7.md) 一致）。

**通过判据**：正例全过；负例 **状态码 + JSON-RPC error.code** 与 tracker / WP2 文档一致（允许 `message` 次要文本差异，**code 必须一致**）。

**失败时收集项**：**tcpdump 禁用**（除非安全审批）；保留 **HTTP 状态行 + 脱敏响应体**。

**推荐目标名**：`a2a_live_smoke`（`ENVIRONMENT` 中绑定上述变量；**默认 CI job 不注册** 或 `DISABLED`）。

---

### 4.4 Tier D — 多 Agent 协同（Online 或双 listen 本机）

**目标**：验证 **多 Card、多 endpoint、能力声明差异** 下的 **发现 + 独立任务**；可选验证 **编排器** 将 Agent A 的 **Artifact / Message** 交给 Agent B 的 **任务创建**（**规范级 handoff** 若未实现，则 **测试脚本扮演 orchestrator**，仍属有效协同测试）。

**环境**：

- `AGENT_A2A_MULTI_LIVE=1`
- `AGENT_A2A_LIVE_AGENT_A_URL`、`AGENT_A2A_LIVE_AGENT_B_URL`（**或** 本机两端口 `http://127.0.0.1:P1`、`P2`）

**测试画像（须在 `tests/fixtures/a2a/agents/README.md` 或 manifest 中 **登记**，实现阶段创建）**：

| 画像 ID | Card `name` | `skills`（示例） | 内部 ToolBus / MCP（最小子集） | 用途 |
|---------|-------------|------------------|--------------------------------|------|
| **T-D-A** | `integration-worker` | `skill.execute` 或等价 tag | 一至两个 **可写或工具调用** 工具（若安全限制则改为 mock 工具） | 生成结构化产物 |
| **T-D-B** | `integration-reviewer` | `skill.review` | **仅只读**工具 + 不同 system 提示（与 WP2.1b **只读并行**策略可联合测） | 消费 A 的输出文本 / Artifact 引用 |

**注意**：**不必** 在 WP2.4 前完成 **全量** MCP 注册；仅 **T-D-A / T-D-B** 所需工具入 **allowlist**（`AGENT_TOOL_ALLOWLIST`），与 [phase-2-plan.md](./phase-2-plan.md) D7 文档一致。

**步骤**：

1. 分别 **GET** 两 Agent 的 Well-Known → 解析 → 断言 **`name` 不同**、**`skills` 数组不同**（至少一个 skill `name` 或 `id` 不同）。
2. 对 A 创建任务：「输出固定格式 JSON 或短文本 **签名**」。
3. **Orchestrator**（测试代码）：读取 A 的任务结果，构造 B 的 **任务创建 params**（将 A 的输出作为 **user message** 或 **Artifact 引用**，形状符合 WP2.1 wire）。
4. 对 B 创建任务：「仅审核：若包含签名 X 则 **pass**，否则 **fail**」— 断言 B 的终态与 **预期** 一致。
5. （可选）**WP2.8**：若 B 的实现为 **主图 + Verifier**，则断言 SSE 中出现 **Verifier 相关事件**（以 WP2.8 文档字段为准）。

**通过判据**：步骤 1–4 无 flake（**同一镜像重复 3 次**均过）；无 **错误端口**、**混用 Card** 导致的 `-32603`。

**失败时收集项**：两 Agent 的 **task id**、**终态 payload**（脱敏）。

**推荐目标名**：`a2a_live_multi_agent`。

---

### 4.5 Tier E — 非功能、安全与回归

**目标**：防止性能回退与 **低级安全错误**（非渗透测试）。

**步骤（最小集）**：

1. **轻量 QPS**：对 **Well-Known GET** 与 **任务创建** 各跑 **R=100** 次（本机），记录 **p95 延迟**；**基线** 写入 `docs/guides/a2a-integration-tests.md` 附录表（首次跑后人工填数，后续 **不超过 2×** 视为通过，或 CI 仅记录不门禁）。
2. **过大 body**：**超过** WP2.1c 预算的请求 → **拒绝或截断** 行为与文档一致，**不**导致进程崩溃。
3. **鉴权负例**（WP2.5）：错误 token → **401/403** 或规范定义行为，**不**泄露栈跟踪到响应体。

**通过判据**：无 crash；鉴权负例 **无敏感信息泄漏**。

---

## 5. 与 WP2.6、WP2.4、WP2.2 的衔接（可操作检查表）

| 序号 | 动作 | 负责人 / 产物 |
|------|------|----------------|
| 1 | **WP2.1** 填满 **tracker §3** 后，在 **本文件 §4** 增加 **附录「方法名速查」**（从 tracker 摘录，避免双源） | 文档 PR |
| 2 | **WP2.6** `manifest.json` 为每个 bundle 标注 **适用 Tier**（A / A+B） | `tests/fixtures/a2a/` |
| 3 | **WP2.4** `AgentClient` 默认路径下，**Tier B** 使用 **同一 `call_jsonrpc` / 解析** 代码路径 | 代码 + 测试 |
| 4 | **WP2.2** `agent-server.md` 写明 **Well-Known**、**JSON-RPC POST**、**SSE** 的 **path** 与 **线程约束** | 运维与测试共用 |
| 5 | **WP2.a2a-test** 第一个合并 PR：**Tier A 扩展 + Tier B 骨架**（`SKIP` 友好） | CI 绿 |

---

## 6. CI 与本地运行策略（强制约定）

| 策略 | 约定 |
|------|------|
| **默认 PR** | **Tier A**：`a2a_contract_json`、`a2a_contract_sse` 与既有 `test_a2a_*`；**不**要求网络。**Tier B**：在 CI 中可对 `a2a_loopback_integration` 注入 `AGENT_A2A_INTEGRATION_LOOPBACK=1`（见根 `.github/workflows/ubuntu.yml` `debug-test-cpp20`） |
| **nightly / main** | 与默认 PR 相同策略即可；亦可单独 job 仅跑 `ctest -R 'a2a_loopback_integration'` |
| **Live** | **仅** `workflow_dispatch` 或 **人工批准** job，注入 `AGENT_A2A_LIVE_*` **Repository secrets** 或 **内网 runner** |
| **密钥** | **禁止** commit；使用 CI secrets 或 `~/.config/agent/live.env`（`.gitignore`） |

---

## 7. 交付物清单（WP2.a2a-test DoD）

- [x] **Tier A**：`tests/fixtures/a2a/synthetic-v1/` + `manifest.json`；`a2a_contract_json`、`a2a_contract_sse`；`a2a_fixture_regen`（维护者）；与既有 `test_a2a_*` 并存。
- [x] **Tier B**：CTest `a2a_loopback_integration`（`AGENT_A2A_INTEGRATION_LOOPBACK=1`）；Well-Known `url` 与动态端口一致；并发 `N=16` 短任务。
- [x] **Tier C**：`a2a_live_smoke`；[`tests/scripts/a2a_live_curl_smoke.sh`](../tests/scripts/a2a_live_curl_smoke.sh)；示例服务 [`agent_server_demo`](../../examples/agent_server_demo.cpp)（`AGENT_BUILD_EXAMPLES=ON`）。
- [x] **Tier D**：[`tests/fixtures/a2a/agents/README.md`](../tests/fixtures/a2a/agents/README.md)（T-D-A/B）；`a2a_live_multi_agent`（默认 `SKIP`）。
- [x] **Tier E**：`a2a_tier_e`（`AGENT_A2A_TIER_E=1`）；Well-Known + SendMessage 各 100 次计时日志；8MiB 大 body；可选 `AGENT_A2A_TIER_E_AUTH_TOKEN` 触发 401 负例。
- [x] **本文件修订记录** 更新版本号与日期。

---

## 8. 附录 A：修订记录

| 日期 | 版本 | 说明 |
|------|------|------|
| 2026-04-05 | 0.1 | 初稿：WP2.a2a-test 分层、依赖矩阵、多 Agent 画像、CI 闸门、与 phase-2-plan / tracker / wp6 衔接 |
| 2026-04-05 | 0.2 | 落地：`synthetic-v1` bundle、`a2a_contract_*`、`a2a_loopback_integration`、`a2a_live_smoke`、`a2a_live_multi_agent`、`a2a_tier_e`、`agent_server_demo`、curl 脚本、agents README；DoD 勾选 |

---

## 9. 附录 B：方法名与路径（摘自 `a2a-spec-tracker.md` §3 / §2，2026-04-05）

权威仍以 **[a2a-spec-tracker.md](./a2a-spec-tracker.md)** 为准；下表仅便於测试与运维速查。

| 类别 | 规范 method 字符串 | 备注 |
|------|-------------------|------|
| 任务创建 | `SendMessage` | `SendMessageResponse`：`task` 或 `message` |
| 流式消息 | `SendStreamingMessage` | 流：`StreamResponse` |
| 任务查询 | `GetTask` | `params.id` |
| 列表 | `ListTasks` | |
| 任务取消 | `CancelTask` | |
| 流式订阅 | `SubscribeToTask` | HTTP+JSON 侧仓库选用 `GET /tasks/sendSubscribe?task_id=`（tracker §2） |
| Push 配置 | `CreateTaskPushNotificationConfig`、`GetTaskPushNotificationConfig`、`ListTaskPushNotificationConfigs`、`DeleteTaskPushNotificationConfig` | |
| 扩展 Card | `GetExtendedAgentCard` | |
| Well-Known | — | `GET /.well-known/agent-card.json` |
| JSON-RPC POST | — | 单一路径：Card 的 `url`（或 `AGENT_SERVER_JSON_RPC_PATH`） |

### 附录 C：Tier E 基线（首次本地跑 `AGENT_A2A_TIER_E=1` 后人工填入）

| 步骤 | 样本数 | p95 / 总耗时（ms） | 日期 / 机器 |
|------|--------|-------------------|-------------|
| GET Well-Known | 100 | （待填） | |
| POST SendMessage | 100 | （待填） | |
