# Scientific Console UI Implementation Plan

> **For agentic workers:** Execute this plan task-by-task in the current session. Do not use subagents unless the user explicitly requests delegation.

**Goal:** Replace the TUI, ImGui, and Web demo layouts with one coherent Scientific Console experience while retaining the complete FS, Web, Expr, Draw, Skills, and MCP runtime.

**Architecture:** A small C++ presentation model converts user turns, streaming output, final results, errors, and `ToolExecutionObserver` events into a thread-safe snapshot consumed by the native UIs. The Web frontend consumes the same normalized SSE semantics through a JavaScript reducer. Each renderer owns layout only; execution remains in the existing GraphExecutor and ToolBus paths.

**Tech Stack:** C++20, nlohmann/json, Dear ImGui/ImPlot, ncursesw, cpp-httplib/SSE, semantic HTML/CSS/vanilla JavaScript, CMake/CTest.

## Global Constraints

- Visual source of truth: `agent_framework/docs/assets/ui/scientific-console-reference.png`.
- Preserve all existing launcher defaults for FS, Web, Expr, Draw, Skills, and Cursor MCP.
- Do not add persistent multi-session storage in this stage.
- Do not expose raw event JSON as the primary user presentation.
- Web, ImGui, and TUI must share state names: `idle`, `running`, `completed`, `failed`, `cancelled`.
- UTF-8/CJK text must remain safe under truncation and terminal wrapping.
- UI completion requires real screenshots and `design-qa.md` with `final result: passed`.

---

### Task 1: Shared presentation model

**Files:**
- Create: `agent_framework/include/agent/ui/presentation_model.hpp`
- Create: `agent_framework/src/ui/presentation_model.cpp`
- Create: `agent_framework/tests/test_ui_presentation_model.cpp`
- Modify: `agent_framework/CMakeLists.txt`

**Interfaces:**
- Produces: `UiRunState`, `UiTurn`, `UiToolActivity`, `UiPresentationSnapshot`, and thread-safe `UiPresentationModel`.
- Consumes: `ToolExecutionEvent`, stream tokens, final-result JSON, errors, and user prompts.

- [ ] Write tests for turn creation, token append, final/error/cancel transitions, tool start/completion pairing, duplicate completion, bounded storage, and UTF-8 truncation.
- [ ] Run `cmake --build build-stage10-ui --target test_ui_presentation_model --parallel 8`; expect the initial compile/test to fail before implementation.
- [ ] Implement the model with snapshot-by-value reads and mutex-protected mutations.
- [ ] Wire the new test target into CTest and rerun it; expect PASS.
- [ ] Commit this independently as `feat: add shared scientific console state`.

### Task 2: Normalize runtime UI events

**Files:**
- Modify: `agent_framework/examples/tui_agent_demo.cpp`
- Modify: `agent_framework/examples/imgui_agent_demo.cpp`
- Modify: `agent_framework/examples/web_ui_demo.cpp`
- Modify: `agent_framework/src/ui/tui_handler.cpp`
- Modify: `agent_framework/src/ui/gui_handler.cpp`
- Modify: `agent_framework/src/ui/web_handler.cpp`
- Modify: `agent_framework/tests/test_imgui_handler_queue.cpp`
- Modify: `agent_framework/tests/test_ui_dispatch_message.cpp`

**Interfaces:**
- Consumes: `CliAgentGraphOptions::tool_execution_observer` and `skill_event_sink`.
- Produces: normalized `tool_started`, `tool_completed`, `run_state`, and skill activity messages.

- [ ] Add failing handler tests asserting stable event kinds and fields.
- [ ] Connect each demo's graph request to the presentation/event sink without changing ToolBus execution.
- [ ] Ensure observer exceptions remain non-fatal and results are summarized with raw JSON retained only for details.
- [ ] Run the handler and presentation tests; expect PASS.
- [ ] Commit as `feat: expose normalized UI execution events`.

### Task 3: Web Scientific Console

**Files:**
- Modify: `agent_framework/examples/web_ui_static/index.html`
- Create: `agent_framework/examples/web_ui_static/styles.css`
- Modify: `agent_framework/examples/web_ui_static/app.js`
- Modify: `agent_framework/examples/web_ui_demo.cpp`
- Create: `agent_framework/tests/scripts/test_web_ui_static.sh`
- Modify: `agent_framework/CMakeLists.txt`

**Interfaces:**
- `POST /ui/run` starts one run and returns 202.
- `POST /ui/cancel` requests cancellation and returns a structured status.
- `/ui/sse?session=default` streams normalized messages.

- [ ] Add static-contract tests for semantic regions, stylesheet loading, responsive hooks, keyboard behavior, safe text insertion, and cancel endpoint wiring.
- [ ] Build the semantic shell: status header, capability/session rail, conversation, activity inspector, and composer.
- [ ] Implement the client reducer for user turns, streaming, final/error/cancel, tool activity, SSE reconnection, autoscroll, and expandable details.
- [ ] Add responsive breakpoints for three-column, collapsed-rail, and single-column modes plus visible focus styles.
- [ ] Wire cancellation through `TaskControl` and prevent concurrent runs.
- [ ] Run shell tests, build `web_ui_demo`, and exercise run/cancel/SSE locally.
- [ ] Commit as `feat: redesign web scientific console`.

### Task 4: ImGui Scientific Console

**Files:**
- Create: `agent_framework/examples/common/imgui_console_view.hpp`
- Create: `agent_framework/examples/common/imgui_console_view.cpp`
- Modify: `agent_framework/examples/imgui_agent_demo.cpp`
- Modify: `agent_framework/CMakeLists.txt`
- Modify: `agent_framework/tests/test_imgui_handler_queue.cpp`

**Interfaces:**
- Consumes: `UiPresentationSnapshot` and callbacks for send, cancel, and quit.
- Produces: a full-window Scientific Console frame with collapsible rails.

- [ ] Move renderer state out of the demo main loop and add headless-testable state transitions.
- [ ] Define graphite/teal style tokens, CJK font fallback, dimensions, focus colors, and reduced-density behavior.
- [ ] Render header, session/capability rail, wrapped conversation turns, activity timeline, details, and multiline composer.
- [ ] Remove permanent sine/helix demo panels; render artifacts only when real result data exists.
- [ ] Wire Enter/Shift+Enter, send, stop, auto-scroll, copy-friendly selection, and resize behavior.
- [ ] Build and run `imgui_agent_demo`; run queue/presentation tests.
- [ ] Commit as `feat: redesign imgui scientific console`.

### Task 5: ncurses Scientific Console

**Files:**
- Modify: `agent_framework/include/agent/ui/tui_handler.hpp`
- Modify: `agent_framework/src/ui/tui_handler.cpp`
- Modify: `agent_framework/examples/tui_agent_demo.cpp`
- Create: `agent_framework/tests/test_tui_handler.cpp`
- Modify: `agent_framework/CMakeLists.txt`

**Interfaces:**
- Consumes: `UiPresentationSnapshot`.
- Produces: wide three-region and narrow single-region layouts with identical status semantics.

- [ ] Add tests for snapshot content, UTF-8 caps, tool activity, run states, and narrow-layout decisions.
- [ ] Add header/status bars, bordered conversation and composer, color pairs with monochrome fallback, and responsive rail visibility.
- [ ] Add Tab focus, PgUp/PgDn scroll, activity toggle, Esc cancel, Enter send, and Ctrl+C quit.
- [ ] Keep wide-character measurement and wrapping correct under CJK text.
- [ ] Build and run `tui_agent_demo` at wide and narrow terminal sizes.
- [ ] Commit as `feat: redesign terminal scientific console`.

### Task 6: Unified launcher, deterministic demo state, and docs

**Files:**
- Modify: `agent_framework/tools/run_ui.sh`
- Modify: `agent_framework/tests/scripts/test_run_ui_launcher.sh`
- Modify: `agent_framework/examples/tui_agent_demo.cpp`
- Modify: `agent_framework/examples/imgui_agent_demo.cpp`
- Modify: `agent_framework/examples/web_ui_demo.cpp`
- Modify: `agent_framework/README.md`

**Interfaces:**
- Adds: `run_ui.sh --demo-state` and matching demo option for all three binaries.
- Preserves: all existing default runtime integrations and raw UI option forwarding.

- [ ] Extend launcher contract tests with `--demo-state` and verify redacted launch plans.
- [ ] Add deterministic scientific conversation/tool fixtures without invoking an LLM.
- [ ] Document startup commands, layout behavior, keyboard controls, cancellation, and demo-state usage.
- [ ] Run launcher tests and all three `--help` smoke tests.
- [ ] Commit as `docs: document scientific console workflows`.

### Task 7: Build, interaction verification, and visual QA

**Files:**
- Create: `design-qa.md`
- Save evidence under: `/tmp/taskflow-scientific-console-qa/`

- [ ] Build `tui_agent_demo`, `imgui_agent_demo`, and `web_ui_demo` from `build-stage10-ui`.
- [ ] Run presentation, handler, launcher, agent-control, and UI integration CTests.
- [ ] Launch each UI with `--demo-state`; verify send/cancel, scrolling, inspector toggles, focus, and resize behavior.
- [ ] Capture Web at 1440x1024, 1024x768, and mobile width; capture ImGui at 1280x720; capture TUI in wide and narrow terminals.
- [ ] Compare the 1440x1024 Web and native captures with the selected source visual in one comparison input.
- [ ] Record and fix every P0/P1/P2 mismatch, then recapture at the same state and viewport.
- [ ] Write `design-qa.md` with source path, screenshot paths, interaction checks, comparison history, remaining P3 items, and exactly `final result: passed` or `blocked`.
- [ ] Run `git diff --check` and report the uncommitted/committed state accurately.

