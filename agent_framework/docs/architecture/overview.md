# Agent Framework 架构概述

## 系统架构

Agent Framework 采用分层架构设计：

1. **应用接口层**：CLI、ImGui、Web 客户端
2. **业务模块层**：LLM Client、ToolBus、Memory、VectorStore、Encoder
3. **核心引擎层**：GraphExecutor、Workflow 库、Taskflow 核心
4. **基础设施层**：向量数据库、事件日志、HTTP 服务器、MCP 服务

## 核心设计原则

1. **声明式数据流**：基于键值驱动 I/O，自动依赖推断
2. **Agent 与工作流统一抽象**：支持嵌套和组合
3. **模块化与可组合性**：每个模块独立开发、测试和复用
4. **多线程并行执行**：工作窃取调度器，自动负载均衡
5. **多端适配**：统一的 Sink 节点接口

## 关键特性

- Agent 循环子图封装
- 知识库作为 Source 节点
- 并行工具调用
- 多模态支持
- 实时流式输出
- MCP 工具集成

详细架构设计请参考：`../../readme/guide_agent.md`

