# Searxng Docker deployment
_Exported on 2026/8/13 at GMT+8 23:23:07 from Cursor (3.15.6)_

---

**User**

参考  @agent_framework/docs/searxng.md 这个在本机部署docker 版的 searxng

---

**Cursor**

先看 `searxng.md` 里的部署步骤和本机 Docker 环境，再按文档把 SearXNG 跑起来。

文档流程清楚了：先确认本机 Docker、端口和是否已有 `~/searxng`，再按文档创建 Compose、启用 JSON API 并验证。

本机没有 Docker 引擎，且 `sudo` 需要密码。先把 `~/searxng` 的 Compose 和配置按文档准备好，同时核对官方镜像要求。

按文档准备 `~/searxng` 配置，并用桌面密码框安装 Docker（麒麟仓库里是 `docker-compose` 1.25，Compose 文件会做成兼容）。

Docker 正在安装，等它装完再拉镜像、启动 SearXNG。

`docker-compose` 和本机较新的 `urllib3` 冲突。先绕过用户 site-packages 再启动；不行就改用 `docker run`。

容器已起来，接下来按文档验证首页和 JSON API。

本机 Docker 版 SearXNG 已按 `agent_framework/docs/searxng.md` 部署完成，并完成 JSON API 验证。

**访问地址**
- Web UI：http://127.0.0.1:8080/
- JSON API：`http://127.0.0.1:8080/search?q=...&format=json`

**验证结果（实测）**
- 首页 HTTP 200
- `q=GNSS+radio+occultation&format=json` 返回 18 条结果，首条来自百度（`cosmic.ucar.edu` 的 GNSS Radio Occultation 页面）

**本机实际做了这些事**
1. 安装了麒麟仓库里的 `docker.io` + `docker-compose`（原先没有 Docker 引擎）
2. 在 `~/searxng` 写好 Compose 和 `settings.yml`（已启用 `json` 格式、关闭 limiter）
3. Docker Hub 超时，改用文档里的 DaoCloud 镜像：`docker.m.daocloud.io/searxng/searxng:latest`
4. 按国内环境关闭了 Google/Bing/DuckDuckGo 等上游，保留百度、Yandex、arXiv、Crossref、OpenAlex
5. 容器已设 `restart: unless-stopped`，开机后会自动拉起

**维护命令**（在 `~/searxng` 下执行）

本机用户级 `urllib3` 和系统 `docker-compose 1.25` 冲突，前面要加 `PYTHONNOUSERSITE=1`：

```bash
cd ~/searxng
PYTHONNOUSERSITE=1 docker-compose logs -f searxng
PYTHONNOUSERSITE=1 docker-compose restart searxng
PYTHONNOUSERSITE=1 docker-compose pull && PYTHONNOUSERSITE=1 docker-compose up -d
```

当前终端如果还没有 `docker` 组权限，先重新登录一次图形会话，或用：

```bash
/usr/bin/sg docker -c 'PYTHONNOUSERSITE=1 docker-compose -f ~/searxng/docker-compose.yml ps'
```

Agent 侧把 SearXNG 基址设成 `http://127.0.0.1:8080` 即可。若要对外暴露、加 Nginx/HTTPS 或 API key，可以说一下目标环境。

---

**User**

一般如何配置呢？

---

**Cursor**

先对照框架里 skills 的 allowlist、`run_skill_script`/`run_skill_cli` 以及 SearXNG 搜索工具的配置入口，再说明一般怎么配。

截图里的状态是预期行为：技能**不能当普通工具名直接调**，必须先被发现，再经 `run_skill_script` / `run_skill_cli` 执行，并且要过两层白名单。未放行时，模型只能用已注册的基础工具（网页搜索、文件、GitHub、GNSS 等）。

## 配置分四层

从外到内依次是：发现技能 → 允许这两个执行入口 → 允许解释器 → 技能包自己声明权限。缺任何一层都会表现为「技能不可用」。

**1. 发现技能目录**

`cli_agent_demo` 默认不扫技能，必须设 `AGENT_SKILLS_DIR`。`cli_agent_skills_demo` / `run_ui.sh` 默认扫 Cursor 双根：`~/.cursor/skills` 与 `~/.cursor/skills-cursor`。目录约定是 `<root>/<skill-folder>/SKILL.md`。

```bash
export AGENT_SKILLS_DIR=/home/Mapoet/.cursor/skills
# 或指到仓库示例
# export AGENT_SKILLS_DIR=/home/Mapoet/projects/taskflow/agent_framework/skills
```

关闭技能用 `AGENT_SKILLS_DISABLED=1`。建议同时打开目录注入，否则模型不知道有哪些 `skill_id`：

```bash
export AGENT_SKILL_INJECT_CATALOG=1
```

**2. 工具白名单（截图说的 allowlist）**

`AGENT_TOOL_ALLOWLIST` 为空或不设 = 不限制。一旦设了非空列表，**没写进去的工具注册/调用都会失败**。要用技能，必须显式加上两个入口：

```bash
export AGENT_TOOL_ALLOWLIST="web_search,web_fetch,fs_read,fs_write,fs_list_dir,run_skill_script,run_skill_cli"
```

这个变量在进程里 `call_once` 缓存，启动后再改环境变量不会生效，必须重启 Agent。

**3. 解释器白名单（真正执行脚本的开关）**

`run_ui.sh` **不会**悄悄打开脚本执行。`AGENT_SKILL_SCRIPT_ALLOWLIST` 为空则**拒绝一切脚本**。本机可信环境一般这样开：

```bash
export AGENT_SKILL_SCRIPT_ALLOWLIST="/bin/sh,/usr/bin/python3"
export AGENT_SKILL_SCRIPT_TIMEOUT_SEC=30
```

Linux 上还需要 `/usr/bin/unshare` 和 `/usr/bin/bwrap`，缺一则 fail closed。

**4. 技能包自己的权限**

`SKILL.md` 的 `permissions.tools` 必须声明会用到的工具，例如：

```yaml
permissions:
  tools: [run_skill_script, run_skill_cli]
  filesystem:
    read: [.]
```

运行时是 **manifest 声明 ∩ 任务 grant**。Agent Loop 只把通过 `SkillPolicyEngine::authorize_tool` 的工具暴露给模型。

## 模型必须怎么调

不能 `call_tool("空间聚类分析")`。正确形态是：

```json
{
  "name": "run_skill_script",
  "arguments": {
    "skill_id": "spatial-clustering",
    "relative_path": "scripts/run.sh"
  }
}
```

`skill_id` 用 Frontmatter 的 `name`（没有则用目录名）。`relative_path` 必须落在该技能目录 jail 内，禁止 `..`。CLI 程序走 `run_skill_cli`，参数约定相同。

## 最小可跑示例

```bash
export AGENT_SKILLS_DIR="$HOME/.cursor/skills"
export AGENT_SKILL_INJECT_CATALOG=1
export AGENT_SKILL_SCRIPT_ALLOWLIST="/bin/sh,/usr/bin/python3"
export AGENT_TOOL_ALLOWLIST="web_search,web_fetch,fs_read,fs_list_dir,run_skill_script,run_skill_cli"

# 可选：离线检查技能包
# skillctl --root "$AGENT_SKILLS_DIR" list
# skillctl --root "$AGENT_SKILLS_DIR" doctor spatial-clustering

./agent_framework/tools/run_ui.sh --ui tui
```

启动后应能看到 skills root 不是 `disabled`，且工具列表里出现 `run_skill_script` / `run_skill_cli`。若仍只有「基础工具」，按这个顺序查：目录是否扫到 → allowlist 是否包含两个入口 → 解释器白名单是否非空 → `SKILL.md` 的 `permissions.tools` 是否声明了它们。

和上一轮部署的 SearXNG 是两条线：本机 `http://127.0.0.1:8080` 已可用，但框架内建 `web_search` 目前走的是 DuckDuckGo HTML，**不会自动改用 SearXNG**。SearXNG 要另接 `web_fetch` 或单独封装工具。若要我按某个具体 skill（空间聚类、选址、去 AI 味）对着本机目录写一份对照配置，指定 skill 目录名即可。

---

**User**

@/home/Mapoet/.cursor/projects/home-Mapoet-projects-taskflow/terminals/2.txt:1017-1022 strategy=truncate意思是没有使用llm来压缩记忆？

---

**Cursor**

先核对记忆压缩的实现：`strategy=truncate` 具体走哪条路径，以及和 LLM 压缩的关系。

是的。`strategy=truncate` 表示这次压缩是**机械截断**，没有走 LLM。

日志对应 `run_memory_compaction` 成功改写历史后的那一行：`strategy_used` 就是实际用上的策略。你这次是 `truncate`，`trigger=auto` 表示字节数过了自动阈值后触发，不是手动 `memory.compact`。

三种策略分别是：

| 日志里的 `strategy=` | 是否调用 LLM | 实际做法 |
|---|---|---|
| `truncate` | 否 | 保留头 `HEAD_KEEP` 条、尾 `TAIL_KEEP` 条，中间整段丢掉，插一条 `system` 标记 |
| `extractive` / `fallback_extractive` | 否 | 从中间抽句子拼摘要，仍不调模型 |
| `structured` | 是 | 用独立的 compaction LLM 总结中间段 |
| `fallback_truncate` | 否 | 选了 LLM/抽取策略但失败后，回退到截断 |

默认就是 `truncate`。只有设了 `AGENT_MEMORY_COMPACTOR=structured`，或 `AGENT_MEMORY_COMPACT_MODE=summarize`，才会先尝试 LLM。若 LLM 不可用或失败，日志会变成 `fallback_extractive` 或 `fallback_truncate`，而不是直接写 `truncate`。

这次数字也能对上「没做语义压缩」：`1734544→1729605` 只少了约 5 KB。`truncate` 默认只留头 1 条、尾 8 条（`AGENT_MEMORY_COMPACT_HEAD_KEEP` / `AGENT_MEMORY_COMPACT_TAIL_KEEP`）。GNSS 这类工具结果单条就很大，丢掉中间几条几乎不降体积，所以会连续自动触发三次。

若要 LLM 压缩，启动前加：

```bash
export AGENT_MEMORY_COMPACTOR=structured
# 或
export AGENT_MEMORY_COMPACT_MODE=summarize
```

并保证 compaction 子 LLM 已注入（`MemoryCompactOptions.sub_llm_client` / `MEMORY_COMPACTION_PROVIDER`）。成功时日志会变成 `strategy=structured`。

---

**User**

	
用独立的 compaction LLM 总结中间段时，采用了什么系统性prompt没？

---

**Cursor**

先看 `structured` 压缩路径里中间段是怎么拼进 LLM 的，以及有没有固定的系统 prompt。

有，而且是**写死在代码里的契约式 prompt**，不是可配置模板，也不会带上主 Agent 的 system / tools / 对话历史。

入口在 `try_summarize_middle`：单独构造一份 `LLMInput`，`tools` 和 `history` 都清空，只走 compaction 子 LLM。

```282:288:agent_framework/src/agent/memory_compaction.cpp
    LLMInput lin;
    lin.system_prompt =
        "You compress a middle segment of an agent conversation. Output exactly one JSON object "
        "with schema_version=1, summary(string), facts(array), open_items(array), and "
        "source_digest(string). Copy the supplied source_digest exactly. No markdown fences.";
    lin.user_prompt = "source_digest=" + digest + "\n\n" + middle;
    lin.tools.clear();
    lin.history.clear();
```

## 系统性体现在三处

**1. 固定 system 指令**

要求模型只输出**一个** JSON 对象，字段是：

| 字段 | 类型 | 作用 |
|---|---|---|
| `schema_version` | 必须为 `1` | 版本门闩 |
| `summary` | string | 中间段摘要 |
| `facts` | array | 抽出的事实 |
| `open_items` | array | 未完成事项 |
| `source_digest` | string | 必须原样抄回输入里的 digest |

禁止 markdown 代码围栏。`source_digest` 是中间段原文的 SHA-256，用来把摘要绑回被压掉的文本；抄错即整次失败。

**2. user 侧是线性化中间段，不是对话回放**

中间段按与 `truncate` 相同的 head/tail 切开，再拼成：

```
<role>: <content>
---
<role>: <content>
---
```

`tool` 消息写的是 `tool_result` 的紧凑 JSON，不是自然语言转述。超长则按 `MEMORY_COMPACTION_MAX_INPUT_BYTES`（默认约 80 KB）做 UTF-8 安全截断。user prompt 前缀是 `source_digest=<hash>\n\n`。

**3. 解析失败就不用这份摘要**

返回值必须能 `json::parse`，且 `schema_version==1`、`summary` 为字符串、`facts`/`open_items` 为数组、`source_digest` 与输入完全一致。任一不满足就 `Failed`，再回退 `extractive` → `truncate`。成功后整份 JSON `dump()` 塞进一条 `system` 消息：

```
[memory_compacted mode=structured trigger=... schema_version=1 source_digest=... policy=wp33-v1]
{"schema_version":1,"summary":"...","facts":[...],"open_items":[...],"source_digest":"..."}
```

## 和主对话 prompt 的隔离

`MemoryCompactionContext` 明确不带 ToolBus / SkillServices。调用时 `temperature` 默认 `0`、`top_p=1`、`stream=false`、`max_retries=0`（profile 可改）。主 Agent 的 system、技能块、工具列表都不会进入这次调用。

早期 WP2.9 文档写的是更简单的 `{"summary":"..."}`。当前实现已换成上面这套 `schema_version=1` 结构化契约；prompt 本身仍是硬编码字符串，没有独立 prompt 文件或 env 可改文案。

---

**User**

model_turn_cannot_verify_task
为啥

---

**Cursor**

先定位 `model_turn_cannot_verify_task` 的触发条件，再对照你这次对话结束时的状态说明原因。

这条 System 报错**不是因为模型说自己没联网/没 MCP**。模型那段「只能估算」是正常回答；`model_turn_cannot_verify_task` 是 Conversation 层在 **Turn 结束后校验失败** 才贴上去的。

## 发生了什么

Web UI 已经把流式答案画成 Agent 气泡。Turn 收尾时，`ConversationEngine` 会校验 `ModelTurnOutcome`：

```94:101:agent_framework/src/conversation/types.cpp
    std::vector<std::string> validate(const ModelTurnOutcome &v)
    {
        std::vector<std::string> e;
        if (v.task_completion_verified)
            e.push_back("model_turn_cannot_verify_task");
        if (v.reason == ModelTurnStopReason::ToolRequested && v.tool_receipt_refs.empty())
            e.push_back("tool_receipt_required");
        return e;
    }
```

硬约束是：**一次模型 Turn 不能把任务标成已验收**。`task_completion_verified=true` 只能由 `TaskClosureController` / 生产 Harness 在任务层签发，Conversation outcome 不得越权。

校验失败后，引擎会改写这次 outcome，并把错误码当成最终文本：

```116:121:agent_framework/src/conversation/conversation_engine.cpp
        if (auto problems = validate(o); !problems.empty())
        {
            o.reason = ModelTurnStopReason::ProviderError;
            o.task_completion_verified = false;
            o.candidate_answer = problems.front();
        }
```

`web_ui_demo` 看到 `reason != EndTurn`，就 `dispatch_error(...)`。前端对非 assistant/user 的错误走 System 气泡，所以你看到：

- Agent：已经流出来的中文回答
- System 18:17：`model_turn_cannot_verify_task`

## 谁把 `verified` 设成了 true

`GraphTurnAdapter` 本身会强制 `task_completion_verified=false`。当前 demo 在 `harness_ready` 时会再包一层 `HarnessTurnAdapter`。交互式 Harness 的 Intake/Cognition/Assurance/Judge 等阶段都是桩，模型只要 `EndTurn`，整条流水线就会 `Completed`，于是：

```125:127:agent_framework/src/conversation/harness_turn_adapter.cpp
    model.task_completion_verified =
        harness::Phase4HarnessRuntime::completion_gate_issues(result.checkpoint).empty();
    return model;
```

门闩为空 → `verified=true` → 立刻被 `validate()` 打回。单测 `test_harness_turn_adapter.cpp` 甚至断言 `outcome.task_completion_verified`，和 Conversation 不变量是对着的。

## 和「没搜索工具」无关

模型说缺实时搜索，只是这一轮的内容。报错在工具调用之后、答案已经写出之后才会出现；没有 MCP 也不会单独触发这个码。

若只想消掉这条 System 提示，需要让 `HarnessTurnAdapter` 在交回 Conversation 时保持 `task_completion_verified=false`（或只在真正的 Task Closure 路径上置位）。我这边是 Ask 模式，不能直接改；要改的话切到 Agent 模式即可。

---

**User**

这又是什么警告？

---

**Cursor**

先定位 `history prefix mismatch` 是在状态合并的哪一步发出的，以及什么情况下会触发。

这是 **WP2.0 会话合并失败**，不是模型答错，也不是 MCP/搜索问题。Turn 已经跑完，往回写会话历史时，前缀对不上，框架按设计 **拒绝静默覆盖**，于是 Web UI 打出这条 System 错误。

## 它在查什么

`run_react_cli_sync` 结束时会把环内终态 `next->history` 合并回会话 `S->history`。合并前有一条不变量：

- 环开始前的历史 `old_hist` 必须原样出现在 `next->history` 的前缀里
- 条数不能变短，`role` / `content` / `tool_result` 必须逐条相等（不比 timestamp）

对不上就失败，**不改** `S->history`：

```310:316:agent_framework/src/graph_executor/graph_executor.cpp
    if (nh.size() < old_hist.size()) {
        return false;
    }
    for (std::size_t i = 0; i < old_hist.size(); ++i) {
        if (!message_equal_for_merge(old_hist[i], nh[i])) {
            return false;
        }
    }
```

`web_ui_demo` 看到 `WorkflowResult.error_message`，就显示成 System：`state merge: history prefix mismatch`。

设计意图是：环内只允许在旧历史**后面追加**本轮 user/assistant/tool。中间被改掉或删掉，说明有两条写入路径在抢同一份 history，不能假装合并成功。

## 你这次最可能的原因

Agent Loop 每轮结束会跑 `maybe_auto_compact_memory`。默认 `truncate` 会丢掉中间消息、插入 `[memory_compacted ...]`，必要时还会按硬上限就地截断旧 tool/user 正文。

这正好破坏前缀：

- 历史变短 → `nh.size() < old_hist.size()`
- 或前几条 `content` / `tool_result` 被改过 → `message_equal_for_merge` 失败

你前面 GNSS 那轮已经出现过连续

`[memory] compact strategy=truncate bytes 1734544->... trigger=auto`

那种超长 tool 结果最容易在本轮结束时再压一次，然后合并失败。

## 和上一条 `model_turn_cannot_verify_task` 的区别

| 报错 | 层 | 含义 |
|---|---|---|
| `model_turn_cannot_verify_task` | Conversation | Turn 结果被标成「任务已验收」，引擎禁止模型 Turn 越权 |
| `state merge: history prefix mismatch` | GraphExecutor | 环内历史和进环前快照对不上，会话写回失败 |

前者是完成权威；这条是历史完整性。答案可能已经流到界面上，但会话 `history` 没有收下本轮增量，下一轮模型可能看不到刚才的工具结果。

## 怎么处理

临时避开：把自动压缩关掉或抬高阈值，例如 `AGENT_MEMORY_AUTO_COMPACT=0`，或加大 `AGENT_MEMORY_SOFT_LIMIT_BYTES` / `AGENT_MEMORY_HARD_LIMIT_BYTES`。

根上要对齐两套语义：压缩可以改 history，合并却要求前缀只读。要么压缩后同步更新 merge 用的 `old_hist` 快照，要么压缩只改「本轮 delta」、不动进环前的前缀。当前代码没有做这一步，所以一压就会炸这条警告。

---

**User**

这个问题怎么解决？1.为何没有指定到正确的skills路径，2，为何SKILL,md没有准确找到并读取？3.这个路径应该必须有ls以及read权限。

---

**Cursor**

先查 `read_skill_resource` 的路径解析、SKILL.md 查找和权限校验，再对照失败原因说明怎么改。

`read_skill_resource` 连失败四次，多半不是磁盘没权限，而是**工具语义和扫描根对不上**。它不是 `ls`/`cat`，也不会去读任意 `SKILL.md`。

## 1. 为何没指到你以为的 skills 路径

`./tools/run_ui.sh` 默认技能根是 **`~/.codex/skills`**，不是 `~/.cursor/skills`，也不是仓库里的 `agent_framework/skills`。

```92:94:agent_framework/tools/run_ui.sh
# Prefer an explicit AGENT_SKILLS_DIR; otherwise use the Codex user Skill root.
DEFAULT_SKILLS_ROOT="${HOME}/.codex/skills"
SKILLS_ROOT="${AGENT_SKILLS_DIR:-}"
```

未传 `--skills-root` 时，启动器会 `export AGENT_SKILLS_DIR=~/.codex/skills`。`SkillServices::from_env()` **只扫这一根**（外加可选 authoring 目录），不再合并 Cursor 双根。

因此：技能在 `~/.cursor/skills/<name>/SKILL.md`，运行时索引里没有这个 `skill_id`，调用直接 `unknown skill_id`。

启动时应对上：

```bash
./tools/run_ui.sh --ui web --port 1357 \
  --skills-root "$HOME/.cursor/skills"
# 或
export AGENT_SKILLS_DIR="$HOME/.cursor/skills"
```

启动日志里的 `skills root:` 必须是你真正放包的目录。可用：

```bash
skillctl --root "$AGENT_SKILLS_DIR" list
```

确认 `skill_id`（Frontmatter `name`，否则目录名）。模型必须用这个 id，不能用中文名或绝对路径。

## 2. 为何 `SKILL.md` 读不到

`read_skill_resource` **不负责读技能说明书**。Registry 启动时已经读过每个子目录的 `SKILL.md` 做索引；正文由路由命中后 `SkillLoader::load_instructions` 注入，不走这个工具。

这个工具只读 **manifest 里声明过的资源**：

```json
{
  "skill_id": "spatial-clustering",
  "relative_path": "references/method.md",
  "kind": "reference"
}
```

三个字段都要有。`SKILL.md` 通常**不会**出现在 `resources.references` 里，于是失败码是 `resource is not declared for requested kind`。

v0（普通 Cursor 包、没有 `api-version: agent.taskflow/v1`）还有一条兼容：未声明该 kind 时，只允许 `references/`、`scripts/` 等前缀。`SKILL.md` 不在这些前缀下，照样拒。

v1 更严：还要求 `ToolCallControl.skill_context` 且 `active_skill_id == skill_id`。路由没命中、或模型去读**另一个** skill 时，返回 `v1 resource access requires matching request context`。测试里无 context 调用就是这个码。

所以模型连续四次 `read_skill_resource(..., SKILL.md)` 会稳定失败。正确用法：

- 读说明书：靠路由 + L2 注入（`AGENT_SKILL_INJECT_CATALOG=1` 让模型看到目录）
- 读附件：把文件写进 `SKILL.md` 的 `resources`，再用 `relative_path` + 匹配的 `kind`
- 跑脚本：`run_skill_script`，不是 `read_skill_resource`

## 3. 「这个路径应该有 ls/read」为什么不够

OS 上能 `ls`/`cat`，只过了内核权限。框架还有三层，任一失败都会显示 Failed：

| 层 | 查什么 | 常见失败 |
|---|---|---|
| `fs_*`（`AGENT_FS_ROOT`） | 工作区监禁 | 技能在 `~/.cursor/skills`，FS 根是仓库，`fs_list_dir`/`fs_read` 进不去 |
| Skill 资源声明 | `relative_path` + `kind` 是否在 manifest | `SKILL.md` 未声明 |
| SkillPolicy（v1） | `permissions.filesystem.read` ∩ 任务 `skill_grants` | grant 为空，或没写 `.` / 包内相对路径 |

v1 读文件时：

```491:495:agent_framework/src/skills/skill_script_tool.cpp
                const auto package = entry->script_jail.value_or(entry->file_path.parent_path());
                SkillPolicyEngine policy(manifest->permissions,
                                         control.skill_context->grants, package);
                const auto read = policy.authorize_filesystem(package / relative_path, false);
                if (!read.allowed) return SkillRuntime::permission_error(read);
```

必须 **manifest 声明了读**，且 **任务 grant 也允许**。Loop 对 v0 会把 grant 抄成 manifest 自身；对 v1 用 `execution_context->skill_grants`，demo 里经常是空的，于是 `path is not contained by both permission sets`。

没有「对 skills 根做 ls」的工具。列表来自 catalog 注入或 `skillctl list`。

v1 包要能读附件，`SKILL.md` 至少要有：

```yaml
api-version: agent.taskflow/v1
kind: Skill
name: spatial-clustering
permissions:
  tools: [read_skill_resource]
  filesystem:
    read: [.]
resources:
  references:
    - id: method
      path: references/method.md
      media-type: text/markdown
```

并保证任务 grant 含 `filesystem_read: ["."]` 和 `tools: ["read_skill_resource"]`。

## 建议怎么处理

先在启动终端看每次 Failed 展开后的 `code`/`error`，对号入座：

- `unknown skill_id` → `--skills-root` 指错，或 `skill_id` 与 `name`/目录不一致
- `resource is not declared for requested kind` → 不要读 `SKILL.md`；去读已声明资源，或补 `resources`
- `v1 resource access requires matching request context` → 先让路由激活该 skill，或先用 v0 包
- `path is not contained by both permission sets` → 补 manifest `filesystem.read` 和任务 grant
- `resource path must be relative` / `must not contain ..` → 不要传绝对路径

不要把 `AGENT_FS_ROOT` 扩到整个 `$HOME` 来「给 ls/read」。技能包读写应走 Skill jail + grant，工作区文件才走 `fs_*`。

展开其中一次 Failed 的 Arguments and result，把 `skill_id`、`relative_path`、`kind` 和返回的 `code` 发出来，可以精确对上是上面哪一条。

---

**User**

@/home/Mapoet/.cursor/projects/home-Mapoet-projects-taskflow/terminals/8.txt:29-296 

---

**Cursor**

这份 `skillctl list` 正好对上前面三次失败：包能扫到，但**没有可被 `read_skill_resource` 读的资源**。

## 这份列表说明什么

`skillctl --root "$HOME/.cursor/skills" list` 成功，说明目录本身可读，索引也建起来了。14 个包都在，`ok: true`。

但每个条目都是：

```json
"references": [],
"scripts": [],
"cli": []
```

也就是 **prompt-only**：只有 `SKILL.md` 正文，没有声明任何附件。`read_skill_resource` 只读已声明资源，对这类包无论传 `SKILL.md` 还是别的路径都会 `resource is not declared for requested kind`。模型连调四次失败，和磁盘权限无关。

`allowedTools` 里的 `Read` / `Write` / `Edit` / `Grep` / `Glob` 是 **Cursor/Claude Code 的工具名**，不是本框架的 `fs_read`、`read_skill_resource`。Agent Loop 的 SkillPolicy 不会把它们映射过去，写在这里等于对 Taskflow 无效。

三条 warn 是 **canonical id 用 Frontmatter `name`，不是目录名**：

| 目录 | 真正的 `skill_id` |
|---|---|
| `humanize-mba-text-skill/` | `humanize-mba-text` |
| `humanize-text/` | `humanize-text-skill` |
| `de-ai-prompt-enhancer/` | `de-AI-writing` |

模型若按目录名去调，会 `unknown skill_id`。

## 和 Web UI 的关系

`run_ui.sh` 默认扫的是 `~/.codex/skills`，不是你刚 list 的 `~/.cursor/skills`。不改启动参数，界面里的 Agent **看不到这份清单**。

```bash
./tools/run_ui.sh --ui web --port 1357 \
  --skills-root "$HOME/.cursor/skills"
```

并建议加上 `AGENT_SKILL_INJECT_CATALOG=1`，让模型用正确的 `id`（`qu-ai-wei`、`de-AI-writing` 等），而不是猜路径。

## 这类技能正确用法

去 AI 味包（`qu-ai-wei`、`shuorenhua`、`humanizer-zh` 等）应靠 **路由命中 → 注入 SKILL.md 正文 → 模型按说明书改写**。不要：

- `read_skill_resource` 读 `SKILL.md`
- `fs_list_dir` / `fs_read` 去扫 `~/.cursor/skills`（在 `AGENT_FS_ROOT` 仓库根之外）

用户说「去 AI 味」时，`match()` 会按 description 选一个；同主题包很多，可能命中 `ai-flavor-remover` 而不是 `qu-ai-wei`。要指定某一个，在对话里写 skill id，或先：

```bash
./build/agent_framework/skillctl --root "$HOME/.cursor/skills" show qu-ai-wei
```

确认说明书已被索引。`triggerKeywords` 目前全是空的，路由只能靠 description 文本重叠，容易选偏。

**结论**：路径和 OS 读权限没问题；缺的是「声明资源 + 启动根指到 Cursor skills + 用注入而不是 read 工具」。这些包本来就不是靠 `read_skill_resource` 工作的。

---

**User**

所以我应该将这类工具名称使用类似Read / Write / Edit / Grep / Glob 是 Cursor/Claude Code 的工具名形式？

---

**Cursor**

先核对框架里 `allowedTools` / `permissions.tools` 是否会把 `Read`/`Write` 这类 Cursor 名称映射到本框架工具。

不要。在本框架里写 `Read` / `Write` / `Edit` / `Grep` / `Glob` **不会**变成 Cursor 那套工具，也**没有**别名映射。

`allowed-tools` 会原样进入 `permissions.tools`，再和任务 grant 做**字符串精确匹配**（或 `*`）。Agent Loop 暴露的是 ToolBus 名：`fs_read`、`fs_write`、`fs_grep`、`web_search`、`read_skill_resource`。`Read` 对不上 `fs_read`，授权直接失败。

| Cursor / Claude Code | Agent Framework 应对 |
|---|---|
| `Read` | `fs_read`（工作区）或 `read_skill_resource`（技能包内已声明文件） |
| `Write` / `Edit` | `fs_write` / `fs_replace` |
| `Grep` / `Glob` | `fs_grep` / `fs_search` / `fs_list_dir` |
| `AskUserQuestion` | 无同名工具；走 HITL / `control_actions`，不要写进 allowed-tools |
| `Bash` | `run_skill_script` / `run_skill_cli`（jail 内），不是任意 shell |

你 list 出来的去 AI 味包几乎都是 **prompt-only**（`references`/`scripts` 为空）。这类技能正确形态是：

```yaml
name: qu-ai-wei
description: ...
# 不必写 allowed-tools
# 不必声明 Read/Write
```

路由命中后注入 `SKILL.md` 正文，模型按说明书改写用户文本即可。写上 Cursor 的 `Read`/`Write` 只会在激活该 skill 后把策略收窄到一堆**不存在的工具名**，反而可能把 `fs_*`、`web_search` 挡掉。

只有技能**真的要调本框架工具**时才写本框架名字，例如：

```yaml
permissions:
  tools:
    - fs_read
    - fs_write
    - read_skill_resource
  filesystem:
    read: [.]
```

同一份 `SKILL.md` 若还要给 Cursor 用，不要混写两套名字。给 Taskflow 的包用 `fs_*`；给 Cursor 的包继续用 `Read`/`Write`。需要两边都能跑，就做两个 frontmatter 变体，或只在 Taskflow 侧覆盖 `permissions.tools`。

**结论**：不要把 Cursor 工具名搬进 Agent Framework。去 AI 味这类技能通常连 `allowed-tools` 都不用写。

---

**User**

那Cursor、Claude Code之间统一这个名字没有？如果没有统一那说明使用这类工具的skill两者不能通用

---

**Cursor**

先核对 Cursor 与 Claude Code 对 `allowed-tools` 的约定，以及有没有官方统一的工具名。

**SKILL.md 文件格式**在 Cursor 和 Claude Code 之间大体统一了；**工具名没有形成可强制执行的统一命名空间**。因此：只靠说明书的 skill 能通用，**依赖 `allowed-tools` 里那些名字的 skill 只能“碰巧通用”**。

## 统一到哪一层

两边都认同一套 Agent Skills 形态：`<dir>/SKILL.md`、YAML frontmatter（`name`、`description`、`allowed-tools`）加 Markdown 正文。你这份 `qu-ai-wei` 甚至写了：

```10:17:/home/Mapoet/.cursor/skills/qu-ai-wei/SKILL.md
compatibility: cursor claude-code codex opencode kiro factory slate hermes
allowed-tools:
  - Read
  - Write
  - Edit
  - Grep
  - Glob
  - AskUserQuestion
```

这表示作者按社区惯例押注「这些宿主都认识这组名字」，不是有一份各方签字的工具目录标准。`allowed-tools` 只是**字符串白名单**，每个运行时拿去和**自己的**工具表做精确匹配。

## Cursor 和 Claude Code 实际有多像

常见六件套 `Read` / `Write` / `Edit` / `Grep` / `Glob`（外加 Claude Code 的 `Bash`）是两边目前最接近的交集，所以大量去 AI 味、写作类 skill 能在 Cursor 和 Claude Code 里共用同一份包。

没有统一、现在就会分叉的包括：

| 能力 | Claude Code | Cursor | 结果 |
|---|---|---|---|
| 改文件 | `Edit` | 有时是 `Edit`，有时是 `StrReplace` | 只写 `Edit` 在部分 Cursor 版本上等于没授权 |
| 终端 | `Bash` | 常为 `Shell` | 名字不同就不能当同一条 |
| 问用户 | `AskUserQuestion` | 不一定同名或未暴露给 skill | 写了可能被静默忽略或拒授权 |
| 网页 | `WebFetch` / `WebSearch` | 另一套或 MCP | 不通用 |

所以：**不是“完全没统一”，而是“核心读写搜有事实标准，其余各写各的”。** 没有规范保证明年还对齐。

## 和「能不能通用」的关系

分三层看：

1. **Prompt-only**（你 list 里大多数：`references`/`scripts` 为空，正文只教怎么改写）  
   Cursor 和 Claude Code **能通用**。`allowed-tools` 即使写了 `Read`，宿主若不强制，也不影响「把说明书注入模型」。

2. **正文或权限依赖具体工具名**（「先 Read `references/xxx.md`」「用 Grep 扫仓库」）  
   只在**两边都注册了同名工具**时通用。这是约定，不是协议。换 `Bash`/`AskUserQuestion` 就容易一边能跑、一边不能。

3. **Agent Framework / Taskflow**  
   **不能**和上面那套通用。这里是 `fs_read`、`fs_write`、`fs_grep`、`read_skill_resource`，没有 `Read`→`fs_read` 映射。同一份 `allowed-tools: [Read, Write, Edit]` 在 Taskflow 里是无效权限，激活后还可能把真正的 `fs_*` 挡掉。

你的判断对了一半：  
**依赖 Cursor/Claude 工具名的 skill，不能当成跨运行时标准包**；  
**Cursor ↔ Claude Code 对那几个常用名目前大体能共用**；  
**加上 Taskflow（以及 Codex 的 `apply_patch`/`shell` 等）就不是同一套名字，不能一份 frontmatter 走天下。**

实务上：给 Cursor/Claude 的包继续写 `Read`/`Write`/`Edit`；给 Taskflow 另写 `permissions.tools: [fs_read, …]`，或拆成两个 SKILL 变体。去 AI 味这类只改用户粘贴文本的，最干净的做法是 **两边都不写 allowed-tools**，只靠说明书，可移植性最好。

---

**User**

那我taskflow是permissions.tools: [fs_read, …]，CC、Codex是@SKILL.md (11-17) ，也不一致

---

**Cursor**

对，**三套运行时认的不是同一份字段，也不是同一组名字**。这不是你配错了，是宿主权限模型就没对齐。

## 同一份 `SKILL.md` 会被怎么读

| 宿主 | 认的字段 | 认的名字 | 不认什么 |
|---|---|---|---|
| Claude Code / Cursor | 顶层 `allowed-tools` | `Read` `Write` `Edit` `Grep` `Glob`… | `permissions.tools`、`fs_read` |
| Codex | 同样多半是 `allowed-tools`（Skills 格式） | 自己的 `shell` / `apply_patch` 等 | `Read` 和 `fs_read` 都不保证能对上 |
| Taskflow | v0：`allowed-tools` **整表覆盖** `permissions.tools`；v1：只认 `permissions.tools` | `fs_read` `fs_write` `read_skill_resource`… | `Read`/`Write`/`Edit` 无别名 |

Taskflow 解析是这样的：没有 `api-version: agent.taskflow/v1` 就算 v0；v0 若写了 `allowed-tools`，会**覆盖** `permissions.tools`：

```385:391:agent_framework/src/skills/skill_manifest.cpp
    if (m.legacy_v0) {
        ...
        if (root.contains("allowed-tools")) m.permissions.tools = strings(root["allowed-tools"]);
    }
```

所以 `qu-ai-wei` 这种 Cursor 包进 Taskflow 后，权限表就是 `[Read, Write, Edit, Grep, Glob, AskUserQuestion]`。激活后 `authorize_tool` 按**精确字符串**拦，`fs_read` 不在表里就会被拒。你在 Taskflow 里再写 `permissions.tools: [fs_read, …]`，**只要仍是 v0 且留着 `allowed-tools`，也会被覆盖掉**。

v1 反过来：只读 `permissions:`，顶层 `allowed-tools` 当未知/扩展字段留着，CC/Cursor 仍可能读它，但 Taskflow 授权只用 `permissions.tools`。

## 这意味着什么

- **说明书**（name / description / 正文）：三边都能用，prompt-only 最可移植。
- **工具权限**：CC/Cursor 一套，Taskflow 一套，Codex 往往又是第三套。没有统一注册表。
- 一份 frontmatter **不能**同时当三边的有效 allowlist，除非某边做映射（Taskflow 现在没有 `Read`→`fs_read`）。

你的判断成立：靠这些工具名的 skill，**不能**当成 CC / Codex / Taskflow 通用包。

## 实际怎么放

去 AI 味、只改用户粘贴文本：三边都**删掉** `allowed-tools`，也不写 `permissions.tools`。只靠注入正文，冲突最小。

真要在 Taskflow 里读工作区 / 技能附件，不要改那份给 CC 用的 `allowed-tools`，而是另做 Taskflow 变体，例如：

```yaml
api-version: agent.taskflow/v1
kind: Skill
name: qu-ai-wei
permissions:
  tools: [fs_read, fs_write, fs_replace, fs_grep, fs_search]
  filesystem:
    read: [.]
    write: [.]
```

和 Cursor 包分开目录或分开 frontmatter。想共目录时，v1 的 `permissions` 给 Taskflow，顶层 `allowed-tools` 留给 CC——但 Codex 仍可能对不上 `Read`。

**一句话**：CC/Codex 的 `allowed-tools: [Read, …]` 和 Taskflow 的 `permissions.tools: [fs_read, …]` 就是两套契约；现在没有统一层，通用的只有 prompt，不是工具白名单。

