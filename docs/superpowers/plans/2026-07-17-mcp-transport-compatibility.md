# MCP Transport Compatibility Implementation Plan

**Goal:** Make Cursor-imported modern stdio MCP servers work reliably in every Agent Framework example, preserve explicit compatibility with legacy Content-Length peers, and make HTTP failures actionable without leaking credentials.

**Architecture:** Use MCP JSON Lines as the stdio default and carry an explicit per-service framing option through ToolBus to MCPClient. Keep the legacy parser behind an opt-in framing value. Start stdio servers in their own process group so failed `npx` launches are fully reaped. Retain the existing Streamable HTTP POST transport while reporting the underlying cpp-httplib error and clearly documenting that legacy SSE is a different transport.

**Tech Stack:** C++17, nlohmann/json, POSIX spawn/poll/process groups, cpp-httplib, CMake/CTest, Bash launcher tests, ncursesw TUI.

---

### Task 1: Lock down modern and legacy stdio framing behavior

**Files:**
- Modify: `agent_framework/include/agent/internal/stdio_framing.hpp`
- Modify: `agent_framework/tests/test_mcp_wp3.cpp`

1. Add failing tests for fragmented JSON Lines, multiple buffered messages, cancellation/timeout, malformed stdout, and size limits.
2. Retain and rename the existing Content-Length tests as legacy coverage.
3. Run `test_mcp_wp3` and confirm the JSON Lines tests fail before implementation.

### Task 2: Carry an explicit framing mode through the MCP API

**Files:**
- Modify: `agent_framework/include/agent/mcp_client/mcp_client.hpp`
- Modify: `agent_framework/src/mcp_client/mcp_client.cpp`
- Modify: `agent_framework/src/mcp_client/stdio_transport.cpp`
- Modify: `agent_framework/src/toolbus/toolbus.cpp`

1. Introduce a public stdio framing enum with JSON Lines as the default.
2. Add MCPClient overloads/options that pass the framing mode to `StdioMCPTransport`.
3. Parse optional per-service `framing: "jsonl" | "content-length"` in Cursor-style configuration and reject unknown values per service.
4. Write newline-delimited JSON by default and dispatch reads to the selected parser.
5. Start the child in a dedicated process group and terminate/reap the group during disconnect.

### Task 3: Add process-level stdio integration tests

**Files:**
- Add: `agent_framework/tests/fixtures/fake_stdio_mcp_server.cpp`
- Add: `agent_framework/tests/test_mcp_stdio_transport.cpp`
- Modify: `agent_framework/CMakeLists.txt`

1. Build a deterministic fake server supporting JSON Lines and legacy Content-Length modes.
2. Exercise initialize, initialized notification, tools/list, and tools/call through the real pipes and transport.
3. Verify invalid framing and failed startup produce bounded, actionable failures.

### Task 4: Restore production-safe timeout defaults

**Files:**
- Modify: `agent_framework/examples/cli_agent_demo.cpp`
- Modify: `agent_framework/examples/cli_agent_skills_demo.cpp`
- Modify: `agent_framework/examples/tui_agent_demo.cpp`
- Modify: `agent_framework/examples/imgui_agent_demo.cpp`
- Modify: `agent_framework/examples/web_ui_demo.cpp`
- Modify: `agent_framework/tools/run_tui.sh`
- Modify: `agent_framework/tests/scripts/test_run_tui_launcher.sh`

1. Stop overriding the transport's 60000 ms default with 1500 ms.
2. Correct startup messages and expose the effective timeout in the launch plan.
3. Extend launcher tests for default and explicit timeout values.

### Task 5: Make HTTP failures precise and safe

**Files:**
- Modify: `agent_framework/src/mcp_client/http_transport.cpp`
- Modify: `agent_framework/tests/test_mcp_wp3.cpp`

1. Include cpp-httplib's categorized error in connection failures.
2. Preserve URL/query/header secrecy in all diagnostic paths.
3. Add a legacy `/sse` guidance message only when the failed configuration indicates that transport, without rejecting a valid Streamable HTTP endpoint merely because of its path.
4. Test error categorization and diagnostic redaction.

### Task 6: Update operator documentation

**Files:**
- Modify: `agent_framework/docs/guides/mcp-spec-tracker.md`
- Modify: `agent_framework/docs/guides/cursor_mcp_json.md`
- Modify: `agent_framework/docs/guides/getting_started.md`
- Modify: `agent_framework/docs/guides/rich-ui.md`
- Modify: `agent_framework/docs/guides/envs_status.md`

1. Correct stdio framing documentation to JSON Lines default plus legacy opt-in.
2. Document timeout behavior, process cleanup, Streamable HTTP versus legacy SSE, and credential hygiene.
3. Add troubleshooting examples for the exact failures observed.

### Task 7: Verify local services and the real TUI

1. Run focused MCP, ToolBus, launcher, and example tests.
2. Configure/build the TUI and run the full relevant CTest suite.
3. Validate cached/installed Context7, Filesystem, and Playwright stdio servers through a sanitized temporary MCP config.
4. Inspect `127.0.0.1:8895` outside the network sandbox. If it offers Streamable HTTP, update the user configuration after backing it up; if it only offers legacy SSE or is not running, report the exact external prerequisite rather than guessing.
5. Launch the real ncurses TUI and capture a screenshot showing a successful, usable startup state.

### Task 8: Review and commit

1. Run `git diff --check`, inspect the complete diff, and confirm unrelated `sum_1_to_100.py` remains untouched.
2. Commit the MCP transport compatibility work as one focused commit.
3. Do not push until explicitly requested.
