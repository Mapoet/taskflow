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

行为概要：流式 token 打印到 **stdout**；Loop 结束后 **Sink** 触发 `CLIHandler::handle_final_result`，默认打印简短 `[result]` 摘要（避免与流式全文重复）。工具行写入 **stderr**（`[tool] name=…`）。`--mock` 预留给 WP1.7，当前会报错退出。

**Cursor MCP**（与 `test_agent_loop_wp5` 一致）：默认在注册 **add** 之后加载 Cursor `mcp.json`；路径为 `--cursor-mcp-json` → 环境变量 `AGENT_TEST_CURSOR_MCP_JSON` → 空则由 `ToolBus` 使用 `AGENT_MCP_CONFIG_PATH` 或 `~/.cursor/mcp.json`。跳过：`--no-cursor-mcp` 或 `AGENT_TEST_SKIP_CURSOR_MCP` / `AGENT_CLI_SKIP_CURSOR_MCP=1`。详细导入日志：`-v` 或 `AGENT_TEST_AGENT_LOOP_DEBUG=1`。

与 `simple_agent` 区别：`cli_agent_demo` 接真实 **WP1.5** 图（`build_cli_agent_graph_with_terminal_sink`）、**ToolBus** 演示工具 **add**、可选 **Cursor MCP**、**SIGINT** 合作式退出。

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

