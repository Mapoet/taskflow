# TUI Skills and Resource Runtime

The TUI can discover, match, read, execute, create, reload, explicitly activate, and deactivate Skills. Management commands execute locally and are not sent to the LLM.

## Start

```bash
agent_framework/tools/run_ui.sh --ui tui \
  --skills-root agent_framework/examples/skills \
  --skill-authoring-root /tmp/taskflow-authored-skills
```

`AGENT_SKILLS_DIR` selects an installed Skill root. Installed packages are treated as read-only by the management API. `AGENT_SKILL_AUTHORING_DIR` is the only location `/skills create` may modify. `AGENT_FS_ROOT` remains the independent workspace filesystem jail.

Use `--no-skills` or `AGENT_SKILLS_DISABLED=1` to disable discovery.

## Commands

```text
/skills list
/skills status
/skills validate research-helper
/skills create my-skill --description A local research workflow
/skills reload
/skills activate research-helper
/skills deactivate
```

`reload` atomically publishes a new Registry generation. A request pins the generation visible when it starts. A reload containing error diagnostics restores the previous entries. Reload is rejected while an explicitly activated Skill owns capability bindings; deactivate first so Tool and Skill-owned MCP cleanup is deterministic.

Activation binds declared Tool, MCP, Prompt, and Template capabilities through `SkillCapabilityRuntime`. Published tools use `skill::<skill-id>::<capability-id>`. Deactivation closes Skill-owned MCP clients and unregisters the binding without affecting globally imported Cursor MCP services.

## Resource namespaces

- `workspace://path`: a file below `AGENT_FS_ROOT`.
- `skill://skill-id/path`: a declared, read-only resource from a pinned Skill package.
- `skill-cache://skill-id/path`: internal writable cache namespace; not injectable into prompts.
- `mcp://service/resource-uri`: a remote MCP Resources namespace. `service` must be registered
  in `ToolBus` and allowed by the pinned `SessionResourceContext`; the remainder is passed
  unchanged to MCP `resources/read` and is never resolved as a local path. For example,
  `@{mcp://filesystem/file:///home/data/input.txt}` sends `file:///home/data/input.txt` to
  the registered `filesystem` server.

MCP resource injection requires the server to advertise the `resources` capability during
`initialize`. Text contents are concatenated in response order and share the existing injection
budget. Binary `blob` contents are rejected with `mcp_resource_binary_not_injectable`; malformed,
empty, unauthorized, unknown-service, and oversized responses remain explicit Tier-A violations.
`AGENT_MCP_RESOURCE_MAX_BYTES` limits the raw MCP response payload (default 256 KiB), while
`AGENT_INPUT_FILE_INJECT_MAX_BYTES` independently limits text admitted into one prompt resource.

Inject local resources with:

```text
Summarize @{workspace://notes/input.md}
Apply @{skill://research-helper/references/checklist.md}
```

Legacy `@file(...)` and `@url(...)` syntax remains supported.

All URI paths must be relative. Empty segments, `.`/`..`, backslashes, absolute paths, unknown schemes, canonical-path escape, undeclared Skill resources, budget overflow, and disallowed MCP services are rejected.

## Lifecycle and recovery

1. Startup discovers configured roots and publishes generation 1.
2. Each request pins a `SessionResourceContext` and Registry snapshot.
3. `/skills reload` scans and validates into a new generation.
4. New requests use the new snapshot; already-running requests retain their old package lease.
5. If activation fails, no partial binding is retained. Use `/skills status` for roots, generation, diagnostics, active Skill, and registered tools.

Global Cursor MCP configuration and Skill-owned MCP descriptors intentionally remain separate trust domains. A global service follows application startup/shutdown policy; a Skill-owned service follows activation/deactivation policy and manifest permission grants.
