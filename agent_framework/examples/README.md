# Agent Framework examples

All default examples build through the `agent_framework_examples` target. Deterministic examples
also run under the `stage10-examples` CTest label.

## Skill platform reference

`skill_platform_demo` is the offline Stage 1–9 reference application:

```bash
./skill_platform_demo /path/to/skills optional-skill-id --jobs 4
```

It emits `agent.taskflow/skill-platform-demo/v1` JSON covering validation, resolved inspection,
permissions, Doctor, isolated tests, cache verification, runtime/audit events, optional typed
configuration, and CycloneDX package metadata. It performs no network access or installation.

`cli_agent_demo`, `imgui_agent_demo`, `tui_agent_demo`, `web_ui_demo`, and `agent_server_demo`
use the same `LiveRuntime` bootstrap contract for LLM defaults, local and built-in tools, Skill discovery,
Cursor MCP import, and structured Skill events. `AGENT_SKILLS_DIR` takes precedence; rich UI applications
otherwise scan Cursor's two default Skill roots.
The fullscreen TUI uses the vendored FTXUI 7.0.1 backend from `3rd-party/FTXUI`; initialize
submodules recursively before enabling `AGENT_BUILD_TUI`.

## Phase 4 operations view

The four interactive demos consume the same display-safe `phase4.operations.v1` projection. Use
`--demo-state` for a deterministic UI acceptance snapshot; this mode still initializes the shared
`LiveRuntime`, but does not claim production workflow execution. Set
`AGENT_UI_INITIAL_VIEW=operations` to open the TUI/ImGui operations view initially. In TUI, key `4`
selects the operations pane. Web exposes the Operations tab and, only in `--demo-state`, a bounded
HITL interaction fixture. Normal runs without an accountable HITL executor fail the action endpoint
closed.

## Focused examples

- `tool_integration`: ToolBus schema validation and asynchronous local invocation.
- `workflow_custom`: custom source/node/sink workflow.
- `agent_server_demo`: Live A2A server; use `tools/run_agent_server.sh` and `configs/server.env.example`.

Examples that require an LLM, remote MCP service, A2A peer, or graphical display are build-tested
offline and run only when their corresponding environment and optional build flag are available.
TUI Skill management is available through `/skills list|status|reload|validate|create|activate|deactivate`.
Use `tools/run_ui.sh --skills-root PATH --skill-authoring-root PATH`; resource injection accepts
`@{workspace://path}` and declared `@{skill://id/path}` references. See
[`docs/guides/skills-resource-runtime.md`](../docs/guides/skills-resource-runtime.md).
