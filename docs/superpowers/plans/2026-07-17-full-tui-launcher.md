# Full-Feature TUI Launcher Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Provide a safe, reproducible one-command launcher that configures, builds, and runs the full `tui_agent_demo` integration surface.

**Architecture:** A Bash launcher owns repository discovery, optional trusted environment-file loading, dependency/configuration preflight, incremental CMake build, secure runtime defaults, and argument forwarding. The C++ TUI remains the application entry point and continues to register built-in FS, WEB, ExprTk, Draw, Skills, and MCP capabilities through the shared graph builder.

**Tech Stack:** Bash, CMake, CTest, ncursesw, existing C++20 Agent Framework.

## Global Constraints

- Never hard-code, print, or commit an API key.
- Default filesystem access is jailed to the repository root.
- HTTP remains disabled unless the user explicitly sets `AGENT_WEB_ALLOW_HTTP=1`.
- Skill script execution remains controlled by `AGENT_SKILL_SCRIPT_ALLOWLIST`.
- Do not install packages or invoke `sudo`; report the exact missing dependency instead.
- Preserve the existing Cursor Skills and MCP discovery behavior.

---

### Task 1: Implement the launcher and local configuration contract

**Files:**
- Create: `agent_framework/tools/run_tui.sh`
- Create: `agent_framework/examples/configs/tui.env.example`
- Modify: `.gitignore`

**Interfaces:**
- Consumes: root CMake options `TF_BUILD_AGENT_FRAMEWORK`, `AGENT_BUILD_EXAMPLES`, and `AGENT_BUILD_TUI`; runtime environment consumed by `tui_agent_demo`.
- Produces: `run_tui.sh` CLI with `--build-dir`, `--build-type`, `--fs-root`, `--env-file`, `--provider`, `--model`, `--prompt`, `--max-iterations`, `--cursor-mcp-json`, `--no-cursor-mcp`, `--no-build`, `--reconfigure`, `--dry-run`, `--verbose`, and `--help`.

- [ ] Write the launcher with strict Bash mode and repository-relative path discovery.
- [ ] Load only an explicitly selected or repository-local trusted `.env.tui` file and export its assignments.
- [ ] Validate tools, TTY/locale, FS root, build type, numeric iteration limit, and provider credentials.
- [ ] Configure `build-tui`, build only `tui_agent_demo`, then replace the shell process with the executable.
- [ ] Add a documented environment template and ignore `.env.tui`.

### Task 2: Add deterministic launcher tests

**Files:**
- Create: `agent_framework/tests/scripts/test_run_tui_launcher.sh`
- Modify: `agent_framework/CMakeLists.txt`

**Interfaces:**
- Consumes: launcher `--dry-run` and validation behavior.
- Produces: CTest `tui_launcher_script` with no network, LLM call, fullscreen terminal, or package installation.

- [ ] Test `bash -n` and help output.
- [ ] Test repository-root defaults and CLI overrides through `--dry-run`.
- [ ] Test invalid build type, iteration count, FS root, and missing credential failures.
- [ ] Test that dry-run output contains no secret value.
- [ ] Register the test under CTest and run it directly and through CTest.

### Task 3: Document the one-command workflow

**Files:**
- Modify: `agent_framework/docs/guides/getting_started.md`
- Modify: `agent_framework/docs/guides/rich-ui.md`

**Interfaces:**
- Consumes: launcher CLI and environment template from Task 1.
- Produces: copy/edit/run instructions, feature matrix, dependency guidance, and security boundaries.

- [ ] Add the minimal one-command path.
- [ ] Document environment configuration, common overrides, enabled tools, and opt-in security gates.
- [ ] Cross-link detailed rich-UI and built-in tool guides.

### Task 4: Build and validate the real TUI

**Files:**
- Verify: `build-tui/agent_framework/tui_agent_demo`
- Verify: temporary screenshot under `/tmp`

**Interfaces:**
- Consumes: launcher and documentation from Tasks 1–3.
- Produces: build/test evidence and a visually inspected real terminal window.

- [ ] Run `run_tui.sh --dry-run` with a redacted offline credential.
- [ ] Configure and build the real TUI target.
- [ ] Run launcher CTest plus relevant TUI/header regression tests.
- [ ] Start the launcher in a real terminal, capture the actual TUI, and verify Chinese text, output pane, input pane, status line, and terminal sizing.
- [ ] Run `git diff --check`, review scope, and commit as `feat: add full-feature TUI launcher`.
