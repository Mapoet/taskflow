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

# 覆盖 provider（会先设置 AGENT_LLM_PROVIDER 再初始化客户端）
./build/agent_framework/cli_agent_demo --provider openai -p "hello"

# 显式指定 mcp.json（不设则按上一段默认路径解析）
./build/agent_framework/cli_agent_demo --cursor-mcp-json ~/.cursor/mcp.json -p "hello"
```

行为概要：流式 token 打印到 **stdout**；Loop 结束后 **Sink** 触发 `CLIHandler::handle_final_result`，默认打印简短 `[result]` 摘要（避免与流式全文重复）。工具行写入 **stderr**（`[tool] name=…`）。`--mock` 预留给 [WP1.7](./phase-1-wp7.md)（**backlog**；计划在 WP1.8 后与其它离线测一并落地），当前会报错退出。

**Cursor MCP**（与 `test_agent_loop_wp5` 一致）：默认在注册 **add** 之后加载 Cursor `mcp.json`；路径为 `--cursor-mcp-json` → 环境变量 `AGENT_TEST_CURSOR_MCP_JSON` → 空则由 `ToolBus` 使用 `AGENT_MCP_CONFIG_PATH` 或 `~/.cursor/mcp.json`。跳过：`--no-cursor-mcp` 或 `AGENT_TEST_SKIP_CURSOR_MCP` / `AGENT_CLI_SKIP_CURSOR_MCP=1`。详细导入日志：`-v` 或 `AGENT_TEST_AGENT_LOOP_DEBUG=1`。程序会在 **stderr** 打印「加载 MCP / REPL ready / running agent loop」；若启动或提问后长时间无 **stdout** 输出，多为 LLM 首包或 HTTP 慢，可调大 `AGENT_HTTP_TIMEOUT_SEC`，或先用 `--no-cursor-mcp` 排除 MCP 初始化耗时。

**具体如何编辑 `mcp.json`（在 `~/.cursor/mcp.json` 上追加、工具命名、`url`/`command` 规则、与 allowlist 关系）**：见 **[Cursor 格式 mcp.json 配置指南](./cursor_mcp_json.md)**。

**内建文件系统工具（`fs_*`）**：若设置环境变量 **`AGENT_FS_ROOT`** 为已存在目录，构图时会额外注册 **`fs_read` / `fs_write` / `fs_list_dir` / `fs_mkdir` / `fs_delete` / `fs_search` / `fs_grep` / `fs_replace`**（路径监禁、配额与确认写删见 **[内建 fs_* 工具](./builtin-fs-tools.md)**）。若仍使用 Cursor 的 **filesystem MCP**，建议二选一或明确两套根目录策略，以免模型混用。

与 `simple_agent` 区别：`cli_agent_demo` 接真实 **WP1.5** 图（`build_cli_agent_graph_with_terminal_sink`）、**ToolBus** 演示工具 **add**、可选 **Cursor MCP**、**SIGINT** 合作式退出。

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
# 运行简单 Agent 示例
./build/examples/simple_agent

# 运行多模态 Agent 示例
./build/examples/multimodal_agent
```

## 下一步

- 阅读 [Skills 与 Harness（渐进式披露）](./skills.md)，了解技能元数据分层加载、与 Tool/MCP 的分工，以及单文件 `SKILL.md` 约定（**运行时 Registry/Loader 将按 `plan-detailed.md` 分阶段落地**，当前示例仍以 workflow + LLM/Tool 占位为主）
- 阅读 [分阶段实施规划（深度）](./plan-detailed.md) 了解 CLI → A2A → RAG 路线与公开协议对齐策略
- 阅读 [API 文档](../api/)了解详细的 API 说明（若已通过 Doxygen 生成）
- 查看 [架构文档](../architecture/)了解系统设计
- 阅读完整的设计文档：`../../../readme/guide_agent.v3.md`（或 `readme/guide_agent.md`）

