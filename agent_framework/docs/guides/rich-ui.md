# WP2.U：富界面（ImGui / TUI / Web）

本文档对应 [phase-2-wpu.md](./phase-2-wpu.md) 与里程碑 **M8**：在 **`UIManager` / `UIHandler`** 之上提供可选的 **Track I（ImGui）**、**Track T（ncurses TUI）** 或 **Track W（浏览器 + SSE）**。**验收**：三条轨道 **完成其一** 即可；**默认推荐 Track I**。

## 共享行为- **流式**：`UIManager::stream_token(session_id, token)` → 各 handler 的 `handle_stream_token`。
- **终稿**：`UIManager::dispatch_final_result(json)` → `handle_final_result`（`final_answer` 等键与 CLI 一致；去重约定见 [phase-1-wp6.md](./phase-1-wp6.md) §4.3）。
- **错误**：`UIManager::dispatch_error(message)`。
- **辅助事件**：`UIManager::dispatch_message(type, payload)` → `handle_aux_event`；v1 `type` 闭集：`tool_start`、`tool_end`（与 ToolBus 接线可后续 PR）。ImGui/Web/TUI 将 aux 统一为 `StreamMessage` 的 `aux:*` 或 SSE `kind: aux`。

## CMake 选项（默认均为 OFF）

| 选项 | 说明 |
|------|------|
| `AGENT_BUILD_IMGUI` | `imgui_agent_demo`；GLFW（FetchContent）+ Dear ImGui（**优先** `3rd-party/imgui` 子模块，否则 FetchContent）。可选 **ImPlot** / **ImPlot3D**：`3rd-party/implot`、`3rd-party/implot3d`（见根目录 `.gitmodules`） |
| `AGENT_BUILD_TUI` | `tui_agent_demo`；**`find_package(Curses REQUIRED)`**，优先 wide ncurses |
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

- **系统包（Debian/Ubuntu）**：`libncurses-dev`（或提供 **ncursesw** 的开发包）。CMake 使用 **`CURSES_NEED_WIDE_CHAR`**，渲染与输入走 **宽字符 API**（`mvwaddnwstr` / `wget_wch`），配合 **`setlocale(LC_ALL, "")`** 与 **UTF-8 按列宽换行**（`wcwidth`），以正确显示 **中文**。
- **Skills / MCP**：与 **`cli_agent_skills_demo`** 对齐——默认合并扫描 `~/.cursor/skills` 与 `~/.cursor/skills-cursor`（可用 **`AGENT_SKILLS_DIR`** 覆写为单根）；默认加载 Cursor **`mcp.json`**（**`--no-cursor-mcp`** 或 `AGENT_CLI_SKIP_CURSOR_MCP` 等跳过）；**`AGENT_SKILL_INJECT_CATALOG`** 可注入技能短表。
- **运行**：全屏 TUI；底栏输入，回车提交；`Ctrl+C` 退出。类 **`TuiHandler`** 位于 `include/agent/ui/tui_handler.hpp`（缓冲 UTF-8 文本，渲染在 demo 内完成）。

## Track W — `web_ui_demo`

- **Skills / MCP**：与 **`cli_agent_skills_demo`** 相同约定（默认 Cursor 技能双路径 +默认 MCP；**`--cursor-mcp-json`** / **`--no-cursor-mcp`**；**`AGENT_SKILL_INJECT_CATALOG`**）。
- **静态资源**：`agent_framework/examples/web_ui_static/`（`index.html`、`app.js`）。
- **路由**：`GET /` → 静态目录；`GET /ui/sse?session=default` → **`text/event-stream`**；`POST /ui/run`，JSON body `{"prompt":"..."}`，返回 **202**后后台跑与 CLI 同构的 ReAct 图。
- **SSE 行格式**：`data: ` + **单行 JSON**；`kind` ∈ `token` | `final` | `error` | `aux`（与 `WebHandler` 一致）。
- **v1 限制**：**单 SSE 连接槽**（多标签页会收到 503）；生产级背压与多会话见 WP2.2 / WP2.5。

**构建宏**：`AGENT_WEB_UI_STATIC_ROOT` 指向上述静态目录（由 CMake 定义）。

## 测试| ID | 说明 |
|----|------|
| **U-1** | `ctest -R ui_dispatch_message_wpu_u1` |
| **U-2** | `ctest -R imgui_handler_queue_wpu_u2`（不创建窗口） |
| **U-3** | 手动：任选 demo，短 prompt → 流式 → 终稿无全文重复 |

## 许可证摘要（第三方）

- **Dear ImGui**：MIT — [https://github.com/ocornut/imgui](https://github.com/ocornut/imgui)
- **GLFW**：zlib/libpng 风格 — [https://github.com/glfw/glfw](https://github.com/glfw/glfw)
- **cpp-httplib**：MIT —仓库 `3rd-party/httplib`
- **ncurses**：X11 风格（以发行版包说明为准）

## M8 核对清单（DoD）

- [ ] **三选一**：`imgui_agent_demo` / `tui_agent_demo` / `web_ui_demo` 可启动并完成一轮对话路径。
- [ ] **默认 CMake** 不强制拉取 ImGui/GLFW（`AGENT_BUILD_*` 默认 OFF）。
- [ ] **U-1** 通过；CI **U-0** 至少一轨 **仅编译** `AGENT_BUILD_IMGUI=ON`（或其它 Track 等价）。
