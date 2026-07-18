# MCP Resources and Branch Convergence Implementation Plan

> **For agentic workers:** Execute this plan inline, task by task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement MCP Resources discovery and reads for sandboxed prompt injection, then merge `2skills` with `upgrade/taskflow-4x` into `skills` and retire the temporary worktree.

**Architecture:** `MCPClient` owns protocol negotiation and typed resource responses. `ToolBus` retains registered MCP clients and routes resource operations by service name. `UserInputPreprocessor` treats the MCP resource identifier as opaque, checks the session allowlist, reads through `ToolBus`, and injects bounded text only.

**Tech Stack:** C++17, nlohmann/json, JSON-RPC 2.0, MCP protocol `2024-11-05`, CMake/CTest, ncurses TUI.

## Global Constraints

- Preserve Stage 11 TUI/Web/ImGui behavior and Stage 12 Skills behavior during merge.
- Preserve tracked `node_modules` content.
- Never resolve an `mcp://` identifier as a local filesystem path.
- Only registered and session-authorized MCP services may provide resources.
- Binary resource payloads are not injected as text.
- Do not push the resulting branch unless separately requested.

---

### Task 1: MCP Resources protocol API

**Files:**
- Modify: `agent_framework/include/agent/mcp_client/mcp_protocol.hpp`
- Modify: `agent_framework/include/agent/mcp_client/mcp_client.hpp`
- Modify: `agent_framework/src/mcp_client/mcp_client.cpp`
- Test: `agent_framework/tests/test_mcp_wp3.cpp`

**Interfaces:**
- Produces: `MCPResource`, `MCPResourceContent`, `MCPResourceListResult`
- Produces: `MCPClient::supports_resources()`, `list_resources()`, `read_resource()`

- [x] Add failing tests for resource capability negotiation, pagination, text reads, binary reads, malformed results, and cancellation.
- [x] Verify the focused test fails before implementation.
- [x] Add `resources/list` and `resources/read` protocol constants and typed result structures.
- [x] Store server capabilities from `initialize`; reject resource calls when `resources` is absent.
- [x] Validate response types and impose finite page/resource limits.
- [x] Run `test_mcp_wp3` and verify all MCP tool tests remain green.
- [x] Commit the protocol slice as part of the cohesive MCP Resources commit.

### Task 2: ToolBus MCP resource routing

**Files:**
- Modify: `agent_framework/include/agent/toolbus/toolbus.hpp`
- Modify: `agent_framework/src/toolbus/toolbus.cpp`
- Test: `agent_framework/tests/test_mcp_wp3.cpp`

**Interfaces:**
- Consumes: the MCPClient resource API from Task 1
- Produces: `ToolBus::has_mcp_service()`, `list_mcp_resources()`, `read_mcp_resource()`

- [x] Add failing tests for service lookup, client lifetime, duplicate registration, unknown service, and resource routing.
- [x] Replace the MCP service-name-only set with a service-to-client map under the existing ToolBus mutex.
- [x] Route list/read operations without exposing local filesystem access.
- [x] Run MCP and ToolBus focused tests.
- [x] Commit the ToolBus slice as part of the cohesive MCP Resources commit.

### Task 3: Opaque MCP resource URI and injection

**Files:**
- Modify: `agent_framework/src/resources/resource_uri.cpp`
- Modify: `agent_framework/src/agent/user_input_preprocessor.cpp`
- Test: `agent_framework/tests/test_resource_runtime.cpp`
- Test: `agent_framework/tests/test_user_input_preprocessor.cpp`

**Interfaces:**
- Consumes: `ToolBus::read_mcp_resource(service, resource_uri, cancellation)`
- Produces: bounded text injection for `@{mcp://service/resource-uri}`

- [x] Add failing URI tests proving local schemes remain jailed while MCP identifiers such as `file:///home/data.txt` remain opaque.
- [x] Add failing injection tests for allow/deny, missing capability, text, binary, malformed, and oversized responses.
- [x] Parse the MCP path as an opaque non-empty identifier with control-character rejection.
- [x] Replace `mcp_resource_injection_requires_resource_api` with ToolBus routing.
- [x] Concatenate text contents within the existing per-resource and aggregate injection limits.
- [x] Return stable structured violation suffixes for unknown, denied, unsupported, binary, malformed, and oversized resources.
- [x] Run the resource runtime and user-input preprocessor tests.
- [x] Commit the injection slice as part of the cohesive MCP Resources commit.

### Task 4: Documentation and full 2skills validation

**Files:**
- Modify: `agent_framework/docs/guides/skills-resources-runtime.md`
- Modify: relevant TUI/Skills usage documentation

- [x] Document local versus MCP resource resolution and example URI forms.
- [x] Document capability, allowlist, size, binary, timeout, and cancellation behavior.
- [x] Build the affected targets.
- [x] Run all Skill unit tests, MCP tests, Cursor MCP import tests, and input preprocessing tests.
- [x] Commit documentation and any test-only fixes as part of the cohesive MCP Resources commit.

### Task 5: Merge to skills

**Files:**
- Resolve as needed: `agent_framework/CMakeLists.txt`
- Resolve as needed: `agent_framework/examples/common/agent_example_bootstrap.hpp`
- Resolve as needed: `agent_framework/examples/tui_agent_demo.cpp`
- Resolve as needed: `agent_framework/tools/run_ui.sh`

- [x] Create `skills` from `upgrade/taskflow-4x`.
- [x] Merge `2skills` with a merge commit.
- [x] Preserve Stage 11 presentation/activity state and Stage 12 Skills commands/options.
- [x] Preserve `--demo-state`, `--skills-root`, authoring, and `--no-skills`.
- [x] Verify the existing main-worktree `node_modules` remains present and unchanged.
- [x] Run `git diff --check` and inspect the merge diff.

### Task 6: Combined validation and cleanup

- [x] Build TUI, Web, and ImGui demos.
- [x] Run MCP, resource, Skill, Cursor import, launcher, static UI, and presentation tests.
- [x] Launch the merged TUI in the real terminal and capture a screenshot showing the combined layout and Skills/Resources state.
- [x] Commit any merge-resolution fixes.
- [x] Remove `/home/Mapoet/projects/taskflow-2skills` through `git worktree remove`.
- [x] Delete the local `2skills` branch and retain `skills` plus `upgrade/taskflow-4x`.
- [x] Verify the final branch and worktree status are clean.
