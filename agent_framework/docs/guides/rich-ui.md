# WP2.U：富界面（ImGui / TUI / Web）

本文档对应 [phase-2-wpu.md](./phase-2-wpu.md) 与里程碑 **M8**：在 **`UIManager` / `UIHandler`** 之上提供可选的 **Track I（ImGui）**、**Track T（FTXUI TUI）** 或 **Track W（浏览器 + SSE）**。三种界面共享同一 Agent、ToolBus、Skills、Resources 与 MCP 启动路径。

## 共享行为

- **流式**：`UIManager::stream_token(session_id, token)` → 各 handler 的 `handle_stream_token`。
- **终稿**：`UIManager::dispatch_final_result(json)` → `handle_final_result`（`final_answer` 等键与 CLI 一致；去重约定见 [phase-1-wp6.md](./phase-1-wp6.md) §4.3）。
- **错误**：`UIManager::dispatch_error(message)`。
- **辅助事件**：`UIManager::dispatch_message(type, payload)` → `handle_aux_event`；v1 `type` 闭集：`tool_start`、`tool_end`（与 ToolBus 接线可后续 PR）。ImGui/Web/TUI 将 aux 统一为 `StreamMessage` 的 `aux:*` 或 SSE `kind: aux`。

## CMake 选项（默认均为 OFF）

| 选项 | 说明 |
|------|------|
| `AGENT_BUILD_IMGUI` | `imgui_agent_demo`；GLFW（FetchContent）+ Dear ImGui（**优先** `3rd-party/imgui` 子模块，否则 FetchContent）。可选 **ImPlot** / **ImPlot3D**：`3rd-party/implot`、`3rd-party/implot3d`（见根目录 `.gitmodules`） |
| `AGENT_BUILD_TUI` | `tui_agent_demo`；仅在启用时加入 vendored **FTXUI 7.0.1**，不依赖系统 curses 包 |
| `AGENT_BUILD_WEB_UI` | `web_ui_demo`；复用仓库 `3rd-party/httplib` |

根目录配置示例：

```bash
cmake -S . -B build -DCMAKE_CXX_STANDARD=20 \
  -DTF_BUILD_AGENT_FRAMEWORK=ON \
  -DAGENT_BUILD_IMGUI=ON
cmake --build build --parallel -t imgui_agent_demo
```

仅 Web：

```bash
cmake -S . -B build -DCMAKE_CXX_STANDARD=20 -DAGENT_BUILD_WEB_UI=ON
cmake --build build --parallel -t web_ui_demo
```

**CI / 无显示环境**：不在无头环境跑 GLFW 窗口；**U-0** 为 **compile-only** 矩阵一轨（见仓库 `.github/workflows/ubuntu.yml` 中 `wpu-imgui-compile`）。可选环境变量 **`AGENT_RICH_UI_CI=compile_only`** 作语义标记。

## Track I — `imgui_agent_demo`

- **系统包（Linux 示例）**：`libgl1-mesa-dev`，以及 GLFW 构建常见依赖：`libxrandr-dev`、`libxinerama-dev`、`libxcursor-dev`、`libxi-dev`（或发行版元包如 `xorg-dev`）。
- **运行时**：每帧从 `ImGuiHandler::drain_messages` 最多处理 **`AGENT_IMGUI_QUEUE_DRAIN_MAX`** 条（默认 **256**）。
- **运行**：配置与 `cli_agent_demo` 相同的 LLM 环境变量后执行 `imgui_agent_demo`；可选 `-p/--prompt` 首轮问题。

## Track T — `tui_agent_demo`

- **后端**：Mapoet/FTXUI 7.0.1，固定为仓库子模块 `3rd-party/FTXUI`。首次构建先运行 `git submodule update --init --recursive 3rd-party/FTXUI`；无需安装 `libncurses-dev`。
- **构建隔离**：`AGENT_BUILD_TUI=OFF` 时不会加入 FTXUI；启用后也关闭其 docs/examples/tests/modules/install/developer-warning 等上游附加目标。
- **响应式布局**：宽终端同时显示 Capabilities、Conversation、Activity；中等宽度保留能力栏并在 Conversation/Activity 间切换；紧凑终端只显示当前页。底部始终保留 UTF-8 输入框、状态和快捷键。
- **输入与导航**：Enter 提交，Escape 取消当前 Agent 运行，Ctrl+C 安全退出；输入框为空时 `1`/`2`/`3` 切换页面，PageUp/PageDown 或鼠标滚轮滚动。终端 resize 由 FTXUI 自动重排。
- **Skills / MCP**：与 **`cli_agent_skills_demo`** 对齐——默认合并扫描 `~/.cursor/skills` 与 `~/.cursor/skills-cursor`（可用 **`AGENT_SKILLS_DIR`** 覆写为单根）；默认加载 Cursor **`mcp.json`**（**`--no-cursor-mcp`** 或 `AGENT_CLI_SKIP_CURSOR_MCP` 等跳过）；**`AGENT_SKILL_INJECT_CATALOG`** 可注入技能短表。
- **状态边界**：`TuiHandler` 与 `UiPresentationModel` 继续作为后端无关的线程安全状态层；FTXUI 组件只存在于 demo 的 view 模块。Agent worker 可取消、可 join，退出时不会遗留 detached 线程。

推荐从仓库根目录使用统一一键启动器。`--ui` 可选择 `tui`、`imgui` 或 `web`；启动器只启用所选界面的 CMake 开关、只构建对应目标，并在运行前检查凭据与文件系统监禁根。TUI 还会检查交互终端：

```bash
cp agent_framework/examples/configs/ui.env.example .env.ui
# 编辑 .env.ui 后任选一种界面：
./agent_framework/tools/run_ui.sh --ui tui
./agent_framework/tools/run_ui.sh --ui imgui
./agent_framework/tools/run_ui.sh --ui web --port 8080
```

不指定 `--ui` 时默认 TUI。旧的 `run_tui.sh` 保留为 `run_ui.sh --ui tui` 的兼容入口。

完整能力与默认安全策略：

| 能力 | 启动器行为 |
|------|------------|
| FS | `AGENT_FS_ROOT` 默认仓库根；`--fs-root` 可缩小或改为其它已存在目录 |
| WEB | `AGENT_WEB_ENABLE=1`；注册搜索、抓取、RSS 与安全归档；HTTP 仍为显式 opt-in |
| ExprTk / Draw | 默认启用；实际可用性仍由构建依赖决定 |
| Skills | 默认 Cursor 双根、L1 catalog；`AGENT_SKILLS_DIR` 可覆写为单根 |
| Skill scripts | 保留 `AGENT_SKILL_SCRIPT_ALLOWLIST` 显式授权，不默认开放解释器 |
| MCP | 默认加载 Cursor `mcp.json`；stdio 默认 JSON Lines；请求超时默认 60000 ms；支持 `--cursor-mcp-json` 与 `--no-cursor-mcp` |
| LLM | 接受标准 OpenAI-compatible / Anthropic 环境；DeepSeek 使用 OpenAI-compatible base URL |
| 构建 | 默认独立 `build-ui` 构建树；增量 CMake；`--no-build` 复用现有二进制，`--reconfigure` 清理选定构建树 |

`--dry-run` 会执行参数、目录、依赖与凭据预检，输出脱敏后的配置和 shell 命令但不构建、不启动。环境文件使用可信 shell 语法；默认自动读取仓库根 `.env.ui`，并兼容旧 `.env.tui`，也可用 `--env-file` 指定。全部参数以 `./agent_framework/tools/run_ui.sh --help` 为准。

## Track W — `web_ui_demo`

- **Skills / MCP**：与 **`cli_agent_skills_demo`** 相同约定（默认 Cursor 技能双路径 +默认 MCP；**`--cursor-mcp-json`** / **`--no-cursor-mcp`**；**`AGENT_SKILL_INJECT_CATALOG`**）。
- **静态资源**：`agent_framework/examples/web_ui_static/`（`index.html`、`app.js`）。
- **路由**：`GET /` → 静态目录；`GET /ui/sse?session=default` → **`text/event-stream`**；`POST /ui/run`，JSON body `{"prompt":"..."}`，返回 **202**后后台跑与 CLI 同构的 ReAct 图。
- **SSE 行格式**：`data: ` + **单行 JSON**；`kind` ∈ `token` | `final` | `error` | `aux`（与 `WebHandler` 一致）。
- **v1 限制**：**单 SSE 连接槽**（多标签页会收到 503）；生产级背压与多会话见 WP2.2 / WP2.5。

**构建宏**：`AGENT_WEB_UI_STATIC_ROOT` 指向上述静态目录（由 CMake 定义）。

## 测试

| ID | 说明 |
|----|------|
| **U-1** | `ctest -R ui_dispatch_message_wpu_u1` |
| **U-2** | `ctest -R imgui_handler_queue_wpu_u2`（不创建窗口） |
| **U-3** | 手动：任选 demo，短 prompt → 流式 → 终稿无全文重复 |
| **T-FTXUI** | `ctest -R 'ftxui_console_view|tui_handler|ui_presentation_model'`；覆盖宽/中/窄、CJK、工具、Skills 与运行状态 |

## 许可证摘要（第三方）

- **Dear ImGui**：MIT — [https://github.com/ocornut/imgui](https://github.com/ocornut/imgui)
- **GLFW**：zlib/libpng 风格 — [https://github.com/glfw/glfw](https://github.com/glfw/glfw)
- **cpp-httplib**：MIT —仓库 `3rd-party/httplib`
- **FTXUI**：MIT — vendored at `3rd-party/FTXUI`

## M8 核对清单（DoD）

- [ ] **三选一**：`imgui_agent_demo` / `tui_agent_demo` / `web_ui_demo` 可启动并完成一轮对话路径。
- [ ] **默认 CMake** 不强制拉取 ImGui/GLFW（`AGENT_BUILD_*` 默认 OFF）。
- [ ] **U-1** 通过；CI **U-0** 至少一轨 **仅编译** `AGENT_BUILD_IMGUI=ON`（或其它 Track 等价）。
