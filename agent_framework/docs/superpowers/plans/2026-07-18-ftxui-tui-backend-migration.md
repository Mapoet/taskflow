# FTXUI TUI Backend Migration Implementation Plan

> **For agentic workers:** Execute this plan inline, task by task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the ncursesw renderer and input loop behind `tui_agent_demo` with a responsive, testable FTXUI 7.0.1 backend while preserving every Agent, MCP, Resource, Skill, and launcher capability.

**Architecture:** Keep `TuiHandler` and `UiPresentationModel` as backend-neutral state adapters. Vendor FTXUI as an optional submodule used only when `AGENT_BUILD_TUI=ON`, place FTXUI DOM/component code in an example-local view module, and let `tui_agent_demo` own the Agent execution controller and safe worker lifecycle.

**Tech Stack:** C++20, FTXUI 7.0.1, CMake/CTest, CLI11, Taskflow, nlohmann/json.

## Global Constraints

- Create branch `FTXUI` from the clean `skills` branch.
- Add `https://github.com/Mapoet/FTXUI.git` at `3rd-party/FTXUI` and lock gitlink `c100eab535db2283b78d30fcb6d082a1f84fb683`.
- Remove the ncurses/ncursesw build and source dependency; do not retain a second TUI backend.
- Preserve `AGENT_BUILD_TUI`, `tui_agent_demo`, `run_ui.sh --ui tui`, CLI options, FS/WEB/Expr/Draw, Cursor MCP, MCP Resources, and Skills behavior.
- FTXUI remains optional and must not build when `AGENT_BUILD_TUI=OFF`.
- UI acceptance requires real terminal screenshots at wide and compact sizes.
- Do not push unless separately requested.

---

### Task 1: Branch and reproducible dependency

**Files:**
- Modify: `.gitmodules`
- Add gitlink: `3rd-party/FTXUI`

- [ ] Create `FTXUI` from `skills`.
- [ ] Add the Mapoet FTXUI submodule at the exact requested path.
- [ ] Lock and verify commit `c100eab535db2283b78d30fcb6d082a1f84fb683`.
- [ ] Verify recursive submodule initialization and commit the dependency slice.

### Task 2: CMake backend replacement

**Files:**
- Modify: `agent_framework/CMakeLists.txt`

**Interfaces:**
- Consumes: `3rd-party/FTXUI/CMakeLists.txt`
- Produces: `tui_agent_demo` linked to `ftxui::ftxui`

- [ ] Add a configure-time contract test for an initialized FTXUI submodule.
- [ ] Disable FTXUI docs, examples, tests, modules, install rules, and developer warnings in the parent build.
- [ ] Add the subdirectory only under `AGENT_BUILD_TUI=ON` with `EXCLUDE_FROM_ALL`.
- [ ] Remove every Curses lookup, include path, library, and error message.
- [ ] Link the existing target to `ftxui::ftxui` and define `AGENT_TUI_BACKEND_FTXUI=1`.
- [ ] Configure and build the dependency slice, then commit it.

### Task 3: Responsive FTXUI console view

**Files:**
- Create: `agent_framework/examples/common/ftxui_console_view.hpp`
- Create: `agent_framework/examples/common/ftxui_console_view.cpp`
- Test: `agent_framework/tests/test_ftxui_console_view.cpp`

**Interfaces:**
- Consumes: `UiPresentationSnapshot`, a Skill status JSON provider, submit/cancel/quit callbacks.
- Produces: `FtxuiConsoleView::run()`, `request_refresh()`, and an in-memory render function for tests.

- [ ] Write failing render tests for wide, medium, compact, CJK, tool, Skill, error, and busy states.
- [ ] Implement a three-panel wide layout and tabbed compact layout using FTXUI DOM.
- [ ] Implement focusable UTF-8 input, pane scrolling, keyboard help, status badges, and restrained terminal colors.
- [ ] Implement thread-safe `Event::Custom` invalidation.
- [ ] Run the render tests and commit the view slice.

### Task 4: Demo controller and lifecycle migration

**Files:**
- Modify: `agent_framework/examples/tui_agent_demo.cpp`
- Test: `agent_framework/tests/test_tui_handler.cpp`

- [ ] Remove ncurses headers, wide-character conversion, window allocation, manual wrapping, and polling input.
- [ ] Preserve the existing bootstrap, MCP import, ToolBus, Skill services, preprocessing, and graph execution path.
- [ ] Route Enter, Escape, Ctrl+C, focus, scrolling, and resize through FTXUI components/events.
- [ ] Replace detached Agent work with a joinable worker; cancel and join before destroying UI/controller state.
- [ ] Preserve all existing CLI flags and deterministic `--demo-state` behavior.
- [ ] Run handler and controller-focused tests and commit the migration slice.

### Task 5: Launcher and backend contracts

**Files:**
- Modify: `agent_framework/tools/run_ui.sh`
- Modify: `agent_framework/tests/scripts/test_run_ui_launcher.sh`
- Modify: `agent_framework/CMakeLists.txt`

- [ ] Expose `FTXUI 7.0.1` as the selected TUI backend in the redacted dry-run plan.
- [ ] Verify launcher compatibility and missing-submodule diagnostics.
- [ ] Verify the built executable has no ncurses/ncursesw linkage.
- [ ] Register the optional FTXUI render test with the `rich-ui` label.
- [ ] Run launcher/static UI tests and commit the contract slice.

### Task 6: Documentation migration

**Files:**
- Modify: `agent_framework/docs/guides/rich-ui.md`
- Modify: `agent_framework/docs/guides/getting_started.md`
- Modify: `agent_framework/docs/guides/phase-2-wpu.md`
- Modify: `agent_framework/examples/README.md`
- Modify: `agent_framework/examples/configs/tui.env.example`

- [ ] Document submodule initialization, build/run commands, layouts, input, shortcuts, and troubleshooting.
- [ ] Remove current ncurses package requirements and mark historical ncurses contracts as superseded.
- [ ] Document that FTXUI is optional and only activated by `AGENT_BUILD_TUI`.
- [ ] Scan active documentation for stale backend claims and commit the docs slice.

### Task 7: Combined build, regression, and visual acceptance

- [ ] Build `tui_agent_demo`, `web_ui_demo`, and `imgui_agent_demo` from the combined branch.
- [ ] Run FTXUI, rich UI, MCP, MCP Resource, input preprocessing, Skill, launcher, and Web static tests.
- [ ] Run `git diff --check` and inspect submodule/status/linkage evidence.
- [ ] Launch a real 160x42 FTXUI terminal and capture the wide layout.
- [ ] Launch a real 90x30 FTXUI terminal and capture the compact layout.
- [ ] Fix visual or interaction defects found in screenshots and rerun affected tests.
- [ ] Complete the plan checklist, commit final fixes, and verify a clean `FTXUI` branch.
