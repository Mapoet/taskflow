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

## 运行示例

```bash
# 运行简单 Agent 示例
./build/examples/simple_agent

# 运行多模态 Agent 示例
./build/examples/multimodal_agent
```

## 下一步

- 阅读 [API 文档](../api/)了解详细的 API 说明
- 查看 [架构文档](../architecture/)了解系统设计
- 阅读完整的设计文档：`../readme/guide_agent.md`

