# 深度分册：文件安全与命令权限

对比三者在 **「谁能在何时访问什么」** 上的工程化程度：调用前策略、路径约束、命令类工具特殊规则。

## 1. agent_framework

### 1.1 工具名白名单

`ToolBus::call_tool` 首次路径上读取 `AGENT_TOOL_ALLOWLIST`（逗号分隔）；若已配置集合且名称不在集合内，返回 `code: tool_not_allowed` 的 JSON future，**不执行工具**（`agent_framework/src/toolbus/toolbus.cpp` 中 `load_allowlist_once` / `is_tool_allowed`）。

**特点**：粗粒度 **按工具名**；无内置「按路径/按命令内容」的二次策略。

### 1.2 Skills 脚本工具（细粒度示例）

`run_skill_script`（`agent_framework/src/skills/skill_script_tool.cpp`）：

- 参数 `relative_path` **禁止** 以 `/` 开头、**禁止** `..`；脚本必须落在 `AGENT_SKILLS_DIR/skill_id/` 前缀下（canonical 校验）。
- 解释器由 `**AGENT_SKILL_SCRIPT_ALLOWLIST`** 枚举允许路径（如 `/bin/sh`、`/usr/bin/python3`），扩展名映射到解释器。
- 超时 `**AGENT_SKILL_SCRIPT_TIMEOUT_SEC**`（默认 30s）。
- 输出长度 cap（如 64KiB 量级）防止撑爆内存。

这是 AF 中少数 **参数级 + 路径 jail** 与 **解释器 allowlist** 齐备的模块；**通用文件读写工具若由用户自行注册 `LocalTool`，框架层不会自动加沙箱**。

### 1.3 MCP

MCP 子进程/传输在 `mcp_client`、`stdio_transport` 等模块；**安全边界取决于远端 MCP 实现与宿主环境**，AF 侧以 **工具名白名单** 为主控手段。

### 1.4 小结（AF）


| 层级   | 现状                          |
| ---- | --------------------------- |
| 调用前  | 工具名 allowlist（可选）           |
| 路径   | Skills 脚本有 jail；其余工具无默认沙箱   |
| 命令执行 | 无内置 Bash；脚本工具有解释器 allowlist |


## 2. humanus-cpp

### 2.1 Filesystem 工具

`Filesystem` 继承 `**BaseMCPTool`**，schema 与 MCP filesystem server 对齐，文档中明确 **「Only works within allowed directories」**，并提供 `**list_allowed_directories`** 子命令（`humanus-cpp/tool/filesystem.h`）。

**实际允许目录**由 **MCP 服务端配置**决定，HU 侧主要是 **协议与提示词** 传递约束，而非 C++ 内核强制路径校验（若 MCP 配置宽松，风险仍存）。

### 2.2 Python 执行

`PythonExecute` 为 **向 MCP 发送代码字符串** 的工具封装（`humanus-cpp/tool/python_execute.h`）；schema 提示使用绝对路径读写文件。**沙箱强度完全取决于所连 MCP 实现**。

### 2.3 小结（HU）


| 层级   | 现状                                      |
| ---- | --------------------------------------- |
| 调用前  | 无统一 `CanUseTool`；依赖工具/MCP               |
| 路径   | MCP filesystem 的 allowed directories 语义 |
| 命令执行 | Python/浏览器等走 MCP 或外部进程                  |


## 3. Claude Code

### 3.1 权限核心

`hasPermissionsToUseTool` 及相关逻辑集中在 `claude-code-source-code/src/utils/permissions/permissions.ts`（体量很大）。导入侧可见：

- Bash / PowerShell / REPL 等 **命令类工具** 的专门分支；
- `**shouldUseSandbox`**、`SandboxManager`（沙箱执行路径）；
- 与 **PermissionMode**、规则持久化、classifier、自动模式等耦合。

即：**调用前拦截 + 可交互确认 + 规则源（settings 等）** 的一体化产品实现。

### 3.2 Bash 相关

`bashPermissions.js`、`shouldUseSandbox.js` 等与 **命令解析、重定向、工作目录** 协同（具体规则以源码为准）。

### 3.3 小结（CC）


| 层级   | 现状                                                      |
| ---- | ------------------------------------------------------- |
| 调用前  | 强：多模式、可审计、可改写 input                                     |
| 路径   | 工作目录、scratchpad、受保护命名空间等（见 `envUtils` / permissions 引用） |
| 命令执行 | 沙箱路径 + 权限决策链                                            |


## 4. 三库并列对照


| 维度         | AF                   | HU                  | CC           |
| ---------- | -------------------- | ------------------- | ------------ |
| **调用前策略**  | 可选工具名白名单             | 弱中心化                | 强中心化 + UI/队列 |
| **路径约束**   | Skills jail；其余靠工具实现  | MCP allowed dirs 描述 | 多层路径与规则      |
| **命令/解释器** | Skills 解释器 allowlist | MCP 决定              | Bash 沙箱 + 权限 |
| **默认安全姿态** | 框架偏中性                | 能力优先                | 产品偏保守        |


## 5. 对 agent_framework 的建议

1. **文档**：在 ToolBus 文档中明确 **「AF 不默认沙箱文件系统；安全由工具实现 + 部署配置负责」**，避免误读。
2. **渐进增强**：为 `LocalTool` 增加可选 `**ToolSandboxHint`** 或元数据，供 Loop 并行策略与 **未来** 策略引擎使用。
3. **企业路径**：实现 `**call_tool` 前置委托**（同步或异步），输入为 `(name, const json&, context)`，输出 allow/deny/replace-json；语义子集即可对齐 CC，而不引入 React。

