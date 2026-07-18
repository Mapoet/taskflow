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

`cli_agent_demo`, `cli_agent_skills_demo`, `cli_a2a_orchestrator_demo`, `imgui_agent_demo`,
`tui_agent_demo`, and `web_ui_demo` use the same example bootstrap contract for Skill discovery,
Cursor MCP import, and structured Skill events. `AGENT_SKILLS_DIR` takes precedence; the
Skills-specific and rich UI applications otherwise scan Cursor's two default Skill roots.

## Focused examples

- `simple_agent`: deterministic Taskflow/Workflow graph.
- `multimodal_agent`: A2A text and structured-data message serialization.
- `tool_integration`: ToolBus schema validation and asynchronous local invocation.
- `workflow_custom`: custom source/node/sink workflow.
- `agent_server_demo`: A2A server used by live integration tests.

Examples that require an LLM, remote MCP service, A2A peer, or graphical display are build-tested
offline and run only when their corresponding environment and optional build flag are available.
TUI Skill management is available through `/skills list|status|reload|validate|create|activate|deactivate`.
Use `tools/run_ui.sh --skills-root PATH --skill-authoring-root PATH`; resource injection accepts
`@{workspace://path}` and declared `@{skill://id/path}` references. See
[`docs/guides/skills-resource-runtime.md`](../docs/guides/skills-resource-runtime.md).
