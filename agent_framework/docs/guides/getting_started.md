# 快速开始指南

## 概述

本指南将帮助您快速上手 Agent Framework。

## 前置要求

- C++17 兼容的编译器（GCC 8.4+, Clang 10+, MSVC 2019+）
- CMake 3.20+
- Taskflow 和 workflow 库（通过 git submodule 包含）

## 构建项目

```bash
# 克隆项目（包含子模块）
git clone --recursive <repository-url>
cd agent_framework

# 构建
./tools/build.sh Release

# 或者手动构建
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

## WP2.U：可选富界面（ImGui / TUI / Web）

里程碑 **M8** 与构建开关、系统依赖、SSE 协议与 DoD 见 **[rich-ui.md](./rich-ui.md)**（默认 CMake **不** 启用；需 `-DAGENT_BUILD_IMGUI` / `AGENT_BUILD_TUI` / `AGENT_BUILD_WEB_UI`）。

### 完整富界面一键启动

统一启动器支持 TUI、ImGui 和 Web 三种界面。TUI 后端为仓库内固定版本的 FTXUI 7.0.1，不需要系统 ncurses 开发包；首次使用需初始化子模块。ImGui 仍需要 GLFW/OpenGL 系统依赖。复制配置模板、填写一种 LLM 凭据后，从仓库根目录一键配置、增量构建并启动：

```bash
cp agent_framework/examples/configs/ui.env.example .env.ui
# 编辑 .env.ui；该文件已被 .gitignore 忽略，勿提交真实 API Key。
git submodule update --init --recursive 3rd-party/FTXUI
./agent_framework/tools/run_ui.sh --ui tui
./agent_framework/tools/run_ui.sh --ui imgui
./agent_framework/tools/run_ui.sh --ui web --port 8080
```

三种界面使用相同的 ReAct graph 与 ToolBus 注册路径。启动器默认把 `AGENT_FS_ROOT` 限制为当前仓库，完整启用内建 **FS / WEB / ExprTk / Draw** 和 Skill catalog，并沿用 Cursor Skills 双根及 MCP 自动发现。常用覆盖：

```bash
# 启动前只检查配置并显示脱敏后的实际命令
./agent_framework/tools/run_ui.sh --ui imgui --dry-run

# 缩小文件系统监禁根，首轮直接提交问题，并跳过 Cursor MCP
./agent_framework/tools/run_ui.sh --ui web --port 9090 \
  --fs-root /absolute/safe/workspace \
  --prompt "检索资料并整理当前目录中的相关文件" \
  --no-cursor-mcp
```

默认只允许 HTTPS；只有显式设置 `AGENT_WEB_ALLOW_HTTP=1` 才允许 HTTP。Skill 脚本执行也不会被启动器静默放开，须在可信配置中显式设置 `AGENT_SKILL_SCRIPT_ALLOWLIST`。旧 `run_tui.sh` 仍可作为 TUI 兼容入口。完整参数见 `./agent_framework/tools/run_ui.sh --help`，工具边界见 [内建 fs_* 工具](./builtin-fs-tools.md) 与 [内建 web_* 工具](./builtin-web-tools.md)。

MCP stdio 默认使用现代 SDK 的 JSON Lines framing，`npx` 首次启动可能需要下载/加载包，因此默认请求超时为 60000 ms；可用 `AGENT_MCP_REQUEST_TIMEOUT_MS` 覆写。旧 Content-Length 服务需在对应条目显式设置 `"framing": "content-length"`，详见 [Cursor MCP 配置](./cursor_mcp_json.md)。

## WP2.1c：上下文预算（可选阅读）

工具结果与注入文本的字节上限、`_af_truncation` 形态与 CTest 说明见 **[context-budget.md](./context-budget.md)**。

## WP1.1 LLMClient 环境变量（库自测）

配置 `AGENT_LLM_PROVIDER`（`openai` 或 `anthropic`）、`OPENAI_API_KEY` / `ANTHROPIC_API_KEY`，可选 `AGENT_OPENAI_BASE_URL`、`AGENT_ANTHROPIC_BASE_URL`、`AGENT_HTTP_TIMEOUT_SEC`、`AGENT_LLM_MAX_RETRIES`、`AGENT_LLM_MODEL`。使用 `LLMClient::from_env()` 注册默认适配器后，须再 `set_prompt_renderer(...)` 才能调用 `invoke`。构建时开启 `BUILD_TESTING` 并运行 `ctest -R llm_client_wp1`（或 `test_llm_client_wp1`）可做无网络的夹具单测。

### 测试自动化排期（WP1.7 backlog）

[WP1.7：示例与测试](./phase-1-wp7.md) 当前为 **BACKLOG**：计划在 [WP1.8：Skills](./phase-1-wp8.md) 接入图与 ToolBus 之后，再集中补齐 **完整** CTest/CI 默认路径（含 **Skills** 回归）。在此之前，`cli_agent_demo` 手测与现有 `tests/test_*` 可作为主要验证手段。

## WP1.6：`cli_agent_demo`（终端 Agent）

从仓库根目录构建并启用 Agent Framework 后，可执行文件通常在 `build/agent_framework/cli_agent_demo`（若从 `agent_framework/build` 单独构建则在同目录下）。

**必填**：与上节相同 LLM 环境变量（`from_env()`）。**可选**：`AGENT_LOG_LEVEL`（`error|warn|info|debug`，默认 `info`）；`-v` / `--verbose` 等价于本次进程内 `debug`。

```bash
# 帮助
./build/agent_framework/cli_agent_demo -h

# 单次查询
./build/agent_framework/cli_agent_demo -p "What is 2+3? Use the add tool if needed."

# 交互 REPL（TTY）
./build/agent_framework/cli_agent_demo

# 多轮 REPL：同一进程内连续提交多行时，WP2.0 起每轮结束会将对话合并进会话 `history`，
# 后续 `>` 对 LLM 可见上文（实现：`GraphExecutor::run_react_cli_sync` + `merge_react_session_state`）。

# WP2.8：第二套 LLM Verifier（可选，`AGENT_VERIFIER=off` 为默认零成本路径）见 [verifier.md](./verifier.md)。

# 覆盖 provider（会先设置 AGENT_LLM_PROVIDER 再初始化客户端）
./build/agent_framework/cli_agent_demo --provider openai -p "hello"

# 显式指定 mcp.json（不设则按上一段默认路径解析）
./build/agent_framework/cli_agent_demo --cursor-mcp-json ~/.cursor/mcp.json -p "hello"
```

行为概要：流式 token 打印到 **stdout**；Loop 结束后 **Sink** 触发 `CLIHandler::handle_final_result`，默认打印简短 `[result]` 摘要（避免与流式全文重复）。工具行写入 **stderr**（`[tool] name=…`）。`--mock` 预留给 [WP1.7](./phase-1-wp7.md)（**backlog**；计划在 WP1.8 后与其它离线测一并落地），当前会报错退出。

**Cursor MCP**（与 `test_agent_loop_wp5` 一致）：默认在注册 **add** 之后加载 Cursor `mcp.json`；路径为 `--cursor-mcp-json` → 环境变量 `AGENT_TEST_CURSOR_MCP_JSON` → 空则由 `ToolBus` 使用 `AGENT_MCP_CONFIG_PATH` 或 `~/.cursor/mcp.json`。跳过：`--no-cursor-mcp` 或 `AGENT_TEST_SKIP_CURSOR_MCP` / `AGENT_CLI_SKIP_CURSOR_MCP=1`。详细导入日志：`-v` 或 `AGENT_TEST_AGENT_LOOP_DEBUG=1`。程序会在 **stderr** 打印「加载 MCP / REPL ready / running agent loop」；若启动或提问后长时间无 **stdout** 输出，多为 LLM 首包或 HTTP 慢，可调大 `AGENT_HTTP_TIMEOUT_SEC`，或先用 `--no-cursor-mcp` 排除 MCP 初始化耗时。

**具体如何编辑 `mcp.json`（在 `~/.cursor/mcp.json` 上追加、工具命名、`url`/`command` 规则、与 allowlist 关系）**：见 **[Cursor 格式 mcp.json 配置指南](./cursor_mcp_json.md)**。在代码侧对 `ToolBus::call_tool` 做调用前策略（放行 / 拒绝 / 改参）时，见 **[工具调用前 Hook（WP2.1d）](./tool-call-hooks.md)**。

**内建文件系统工具（`fs_*`）**：若设置环境变量 **`AGENT_FS_ROOT`** 为已存在目录，构图时会额外注册 **`fs_read` / `fs_write` / `fs_list_dir` / `fs_mkdir` / `fs_delete` / `fs_search` / `fs_grep` / `fs_replace`**（路径监禁、配额与确认写删见 **[内建 fs_* 工具](./builtin-fs-tools.md)**）。若仍使用 Cursor 的 **filesystem MCP**，建议二选一或明确两套根目录策略，以免模型混用。

**内建网络工具（`web_*`）**：若 **`AGENT_WEB_ENABLE=1`** 且构建已链接 OpenSSL，则注册 **`web_search` / `web_fetch` / `web_rss_feed` / `web_fetch_archive`**。`web_search` 默认通过 `AGENT_WEB_SEARXNG_URL`（默认 `http://127.0.0.1:8080`）检索，并受控解析每项结果正文；调用参数可选择 `duckduckgo`，fallback 默认关闭。完整配置、安全边界与返回结构见 **[内建 web_* 工具](./builtin-web-tools.md)**。若设置 **`AGENT_NEWS_SOURCES_JSON`**，还会注册 **`web_configured_source`**。工具名仍受 **`AGENT_TOOL_ALLOWLIST`** 约束。

**内建表达式工具（`expr_*`，ExprTk）**：若构建包含 **ExprTk**（存在 **`exprtk.hpp`**）且 **`AGENT_EXPR_ENABLE`** 未设为关闭值（**`0` / `false` / `off` / `no`**；未设置时默认开启），则注册 **`expr_eval` / `expr_validate` / `expr_batch_eval`**（详见 **[内建 expr_* 工具](./builtin-exprtk-tools.md)**）。常用变量：**`AGENT_EXPR_MAX_EXPR_BYTES`**（默认 `16384`）、**`AGENT_EXPR_MAX_LOOP_ITERS`**、**`AGENT_EXPR_PARSER_STACK_DEPTH`**、**`AGENT_EXPR_PARSER_NODE_DEPTH`**、**`AGENT_EXPR_DISABLE_CONTROL_FLOW`**。若设置 **`AGENT_TOOL_ALLOWLIST`**，须将 **`expr_eval,expr_validate,expr_batch_eval`** 一并列入，否则注册会失败。

**内建绘图工具（`draw_*`，canvas_ity + stb）**：当编译单元同时具备 **canvas_ity** 与 **`stb_image_write.h`** 时，若 **`AGENT_DRAW_ENABLE`** 未设为关闭值（**`0` / `false` / `off` / `no`**；未设置时默认开启），则注册 **`draw_render`**（内存 PNG → **`png_base64`**）与 **`draw_export`**（在 **`AGENT_FS_ROOT`** 下原子写 PNG，语义对齐 **`fs_write`** 的确认覆盖）。门闩与模板说明见 **[内建 draw_* 工具](./builtin-draw-tools.md)**（**`AGENT_DRAW_MAX_WIDTH`** / **`AGENT_DRAW_MAX_HEIGHT`**、**`AGENT_DRAW_MAX_PIXELS`**、**`AGENT_DRAW_MAX_COMMANDS`**、**`AGENT_DRAW_MAX_OUTPUT_BYTES`**）。子模块初始化示例：`git submodule update --init 3rd-party/canvas_ity 3rd-party/stb`。若设置 **`AGENT_TOOL_ALLOWLIST`**，须将 **`draw_render`** 与 **`draw_export`** 一并列入，否则注册会失败。离线回归：**`ctest -R draw_tools`**。

`cli_agent_demo` 接真实 **WP1.5** 图（经 **WP2.0** `GraphExecutor::run_react_cli_sync` 统一构图与运行）、**ToolBus** 演示工具 **add**、可选 **Cursor MCP**、**SIGINT** 合作式退出。

## WP1.8：Skills（可选）

- **`cli_agent_demo`**：仅当设置 **`AGENT_SKILLS_DIR`**（单根递归扫描）时启用 Skills。
- **`cli_agent_skills_demo`**（构建目标与 demo 并列）：**默认**合并扫描 **`~/.cursor/skills`** 与 **`~/.cursor/skills-cursor`**（目录存在才参与）；若设置 **`AGENT_SKILLS_DIR`**，则与 demo 相同 **只使用该单目录**，便于覆写。二者均会注册 **`run_skill_script`**；脚本 jail 为 **各扫描根下 `<skill_id>/`**（若该目录存在）。

相关环境变量：

| 变量 | 作用 |
|------|------|
| `AGENT_SKILLS_DIR` | **单根**技能目录（`cli_agent_demo` 未设置则关闭；`cli_agent_skills_demo` 未设置则用 Cursor 双路径） |
| `AGENT_SKILL_CONTEXT_MAX_CHARS` | L2 正文注入上限，默认 `8000` |
| `AGENT_SKILL_ROUTER` | 设为 `off` / `0` / `false` 关闭路由 |
| `AGENT_SKILL_SCRIPT_ALLOWLIST` | 逗号分隔解释器路径（如 `/bin/sh,/usr/bin/python3`）；**空则拒绝一切脚本执行** |
| `AGENT_SKILL_SCRIPT_TIMEOUT_SEC` | 子进程超时秒数，默认 `30`（POSIX） |
| `AGENT_SKILL_INJECT_CATALOG` | 非空/`1`/`true` 时在 system 末尾追加已索引技能短表，便于模型使用正确 canonical（配合 `run_skill_script`） |
| `AGENT_SKILL_CATALOG_MAX_CHARS` | 目录摘要最大字符，默认 `2048` |

仓库示例：`agent_framework/skills/demo/SKILL.md` 与 `agent_framework/skills/demo/hello.sh`；可将 `AGENT_SKILLS_DIR` 指到 `agent_framework/skills` 试跑（每技能为 **`<root>/<folder>/SKILL.md`**）。

**Frontmatter 与 Cursor 对齐**：优先 **`name:`** 作为 canonical（与目录名一致为佳）；可保留 **`id:`**（legacy）；**无二者** 时用 **目录名**。多行描述使用 **`description: >-`** 等块标量。**`disable-model-invocation: true`** 的技能不参与自动路由，仍可被工具显式引用。

## 运行示例

```bash
# Live CLI 与 Live A2A Server
./build/agent_framework/cli_agent_demo -p "hello"
./build/agent_framework/agent_server_demo --help

# 富 UI 目标需使用对应 CMake 开关构建
./build/agent_framework/tui_agent_demo
./build/agent_framework/web_ui_demo
./build/agent_framework/imgui_agent_demo
```

## 下一步

- 阅读 [Skills 与 Harness（渐进式披露）](./skills.md)，了解技能元数据分层加载、与 Tool/MCP 的分工，以及单文件 `SKILL.md` 约定（**运行时 Registry/Loader 将按 `plan-detailed.md` 分阶段落地**，当前示例仍以 workflow + LLM/Tool 占位为主）
- 阅读 [分阶段实施规划（深度）](./plan-detailed.md) 了解 CLI → A2A → RAG 路线与公开协议对齐策略
- 阅读 [API 文档](../api/)了解详细的 API 说明（若已通过 Doxygen 生成）
- 查看 [架构文档](../architecture/)了解系统设计
- 阅读完整的设计文档：`../../../readme/guide_agent.v3.md`（或 `readme/guide_agent.md`）
