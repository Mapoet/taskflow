# Agent Framework

基于 Taskflow `workflow` 库的高性能多模态智能代理框架，采用纯 C++17 实现。

## 项目概述

Agent Framework 是一个高性能、多模态的智能代理框架，充分利用 Taskflow `workflow` 的声明式 API、控制流节点和并行执行能力，实现了：

- **LLM 节点多源输入融合**：系统提示词、用户提示词、知识库上下文、记忆、工具列表、多模态输入的统一整合
- **Agent 循环子图封装**：使用 `create_loop_decl` 将 Agent 的决策-执行循环封装为可复用的节点
- **知识库作为 Source 节点**：提供统一的查询 API，支持延迟加载和按需检索
- **并行工具调用**：通过 `create_for_each` 实现工具调用的并行执行
- **多端输出适配**：统一的 Sink 节点接口，支持 CLI、ImGui 和 Web 客户端
- **事件溯源与监控**：完整的事件日志和性能分析支持

## 核心特性

1. **声明式数据流**：基于键值驱动 I/O，自动依赖推断
2. **Agent 与工作流嵌套**：支持多层嵌套和组合
3. **多线程并行执行**：工作窃取调度器，自动负载均衡
4. **多模态支持**：文本、图像、音频、视频的统一处理
5. **实时流式输出**：支持 SSE 和 WebSocket
6. **MCP 工具集成**：统一管理本地函数、MCP 服务和外部 API

## 前置要求

- **编译器**：GCC 8.4+, Clang 10+, MSVC 2019+
- **C++ 标准**：C++17（推荐 C++20）
- **CMake**：3.20+
- **Git**：用于管理子模块

## 快速开始

### 克隆项目

```bash
# 克隆项目（包含子模块）
git clone --recursive <repository-url>
cd agent_framework

# 如果已经克隆，需要初始化子模块
git submodule update --init --recursive

# 或使用工具脚本
./tools/init_submodules.sh
```

### 构建项目

```bash
# 构建 Release 版本
./tools/build.sh Release

# 或手动构建
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

### 运行示例

```bash
# 运行简单 Agent 示例
./build/examples/simple_agent

# 运行多模态 Agent 示例
./build/examples/multimodal_agent
```

## 目录结构

```
taskflow/                       # 项目根目录
├── .cursorrules                # Cursor IDE 开发规则（位于根目录）
├── .gitmodules                 # Git 子模块配置（位于根目录）
│
├── agent_framework/            # Agent Framework 项目目录
│   ├── CMakeLists.txt          # 主构建文件
│   ├── README.md               # 项目说明
│   ├── LICENSE                 # 许可证
│   │
│   ├── include/                # 公共头文件
│   │   └── agent/
│   │       ├── llm_client.hpp  # LLM 客户端接口
│   │       ├── toolbus.hpp     # ToolBus 接口
│   │       ├── memory.hpp      # Memory 接口
│   │       ├── vectorstore.hpp # VectorStore 接口
│   │       ├── graph_executor.hpp  # GraphExecutor 接口
│   │       ├── ui_manager.hpp  # UI 管理器接口
│   │       └── types.hpp       # 公共数据结构
│   │
│   ├── src/                    # 实现文件
│   │   ├── llm_client/         # LLM 客户端模块
│   │   ├── toolbus/            # ToolBus 模块
│   │   ├── memory/             # Memory 模块
│   │   ├── vectorstore/        # VectorStore 模块
│   │   ├── graph_executor/     # GraphExecutor 模块
│   │   └── ui/                 # UI 适配模块
│   │
│   ├── examples/               # 示例程序
│   │   ├── simple_agent.cpp
│   │   ├── multimodal_agent.cpp
│   │   ├── tool_integration.cpp
│   │   └── workflow_custom.cpp
│   │
│   ├── tests/                  # 单元测试
│   ├── tools/                  # 工具脚本
│   │   ├── build.sh
│   │   ├── run_tests.sh
│   │   ├── profile.sh
│   │   └── init_submodules.sh
│   │
│   └── docs/                   # 文档
│       ├── api/                # API 文档
│       ├── guides/             # 使用指南
│       └── architecture/       # 架构文档
│
├── 3rd-party/                  # 第三方依赖（Git Submodules，与 Taskflow 共享）
│   ├── nlohmann_json/          # JSON 库（Header-Only）
│   ├── httplib/                # HTTP 服务器库（Header-Only）
│   ├── websocketpp/            # WebSocket 库（Header-Only）
│   └── faiss/                  # Faiss 向量数据库（可选）
│
├── workflow/                   # Workflow 库
├── taskflow/                   # Taskflow 核心库
└── readme/                     # 文档
    └── guide_agent.md          # Agent Framework 设计文档
```

## 第三方依赖

所有第三方库通过 Git Submodules 管理，位于项目根目录的 `3rd-party/` 目录中（与 Taskflow 使用相同的目录结构）：

- **nlohmann/json**：JSON 解析和生成（Header-Only）
- **httplib**：轻量级 HTTP 服务器库（Header-Only）
- **websocketpp**：WebSocket 库（Header-Only）
- **faiss**：高性能向量检索库（可选，需要编译）
- **imgui**（`3rd-party/imgui`）：Dear ImGui；`AGENT_BUILD_IMGUI=ON` 时优先使用子模块，否则 CMake FetchContent
- **implot**（`3rd-party/implot`）：2D 图表（epezent/implot）；子模块存在时 `imgui_agent_demo` 自动链接
- **implot3d**（`3rd-party/implot3d`）：3D 图表（brenocq/implot3d）；仅依赖 ImGui

上述三项已登记在仓库根目录 `.gitmodules` 中；拉取含子模块指针的提交后，在根目录执行 `git submodule update --init --recursive`（或对各路径单独 `init`/`update`）。若本地仓库尚无子模块 gitlink，请在可访问 GitHub 的环境下于根目录执行 `git submodule add <url> 3rd-party/<name>` 各一次并提交，再按上式更新。

### 管理子模块

```bash
# 在项目根目录初始化所有子模块
cd /path/to/taskflow
git submodule update --init --recursive

# 或在 agent_framework 目录使用工具脚本
cd agent_framework
./tools/init_submodules.sh

# 更新所有子模块到最新版本
git submodule update --remote

# 查看子模块状态
git submodule status
```

## 使用示例

### 简单 Agent 示例

```cpp
#include <agent/core/types.hpp>
#include <workflow/nodeflow.hpp>
#include <taskflow/taskflow.hpp>
#include <iostream>

namespace wf = workflow;

int main() {
    tf::Executor executor(std::thread::hardware_concurrency());
    wf::GraphBuilder builder("simple_agent");

    // 创建系统提示词源节点
    auto [sys_node, _] = builder.create_typed_source(
        "SystemPrompt",
        std::make_tuple(std::string("你是一个有用的助手。")),
        {"prompt"}
    );

    // 创建用户输入源节点
    auto [user_node, _] = builder.create_any_source(
        "UserInput",
        std::unordered_map<std::string, std::any>{
            {"query", std::any{std::string("你好")}}
        }
    );

    // 创建 LLM 节点
    auto [llm_node, _] = builder.create_any_node(
        "LLM",
        {{"SystemPrompt", "prompt"}, {"UserInput", "query"}},
        [](const auto& inputs) {
            // LLM 调用逻辑
            // ...
        },
        {"final_answer"}
    );

    // 创建输出 Sink
    builder.create_any_sink(
        "Output",
        {{"LLM", "final_answer"}},
        [](const auto& outputs) {
            std::string answer = std::any_cast<std::string>(outputs.at("final_answer"));
            std::cout << answer << std::endl;
        }
    );

    // 执行工作流
    builder.run(executor);
    return 0;
}
```

## 模块说明

### LLM Client 模块

提供与各种 LLM 服务通信的接口，支持 OpenAI、Anthropic、Gemini、vLLM 等。

### ToolBus 模块

统一管理所有工具（本地函数、MCP 服务、外部 API），提供统一的调用接口。

### Memory 模块

提供短期和长期记忆的存储和查询接口，基于事件溯源模式。

### VectorStore 模块

封装向量数据库，实现多模态数据的存储和检索。

### GraphExecutor 模块

基于 Taskflow `workflow` 构建和执行工作流图。

### UI Manager 模块

为不同客户端（CLI、ImGui、Web）提供统一的输出接口。

## 开发规范

请参考 `.cursorrules` 文件中的详细开发规范。

主要规范包括：
- 代码风格和命名规范
- Workflow API 使用规范
- 模块实现规范
- 线程安全规范
- 错误处理规范
- 测试规范

## 文档

- **设计文档**：`../readme/guide_agent.md`
- **API 文档**：`docs/api/`（使用 Doxygen 生成）
- **使用指南**：`docs/guides/`
- **架构文档**：`docs/architecture/`

## 许可证

[待定]

## 贡献

欢迎提交 Issue 和 Pull Request。

## 联系方式

[待定]
