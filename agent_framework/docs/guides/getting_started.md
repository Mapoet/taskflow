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

配置 `AGENT_LLM_PROVIDER`（`openai` 或 `anthropic`）、`OPENAI_API_KEY` / `ANTHROPIC_API_KEY`，可选 `AGENT_OPENAI_BASE_URL`、`AGENT_ANTHROPIC_BASE_URL`、`AGENT_HTTP_TIMEOUT_SEC`、`AGENT_LLM_MAX_RETRIES`、`AGENT_LLM_MODEL`。使用 `LLMClient::from_env()` 注册默认适配器后，须再 `set_prompt_renderer(...)` 才能调用 `invoke`。构建时开启 `BUILD_TESTING` 并运行 `ctest -R llm_client_wp1`（或 `test_llm_client_wp1`）可做无网络的夹具单测，与 `cli_agent_demo` 无关。

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

