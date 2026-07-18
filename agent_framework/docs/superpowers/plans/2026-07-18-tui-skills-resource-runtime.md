# Stage 12: TUI Skills and Unified Resource Runtime

**Goal:** Turn one-shot Skill discovery into a safe, observable management and reload runtime without collapsing workspace, installed Skill, cache, and MCP trust boundaries.

**Architecture:** A URI/resource-context layer names isolated domains. `SkillManager` owns registry reload, authoring, activation, and capability bindings. TUI `/skills` commands are local operations and never reach the LLM. Running requests pin a registry generation; later requests see atomically published reloads.

**Tech stack:** C++20, nlohmann/json, existing SkillRegistry/SkillLoader/SkillCapabilityRuntime/ToolBus, CLI11, ncursesw, CMake/CTest.

---

## Task 1: Resource URI and session context

Create `resource_uri` and `session_resource_context` headers/sources plus `test_resource_runtime.cpp`. Test parsing, normalization, traversal, unknown schemes, workspace/Skill resolution, and canonical jail enforcement before implementation.

## Task 2: SkillManager lifecycle

Create `skill_manager` and tests. Cover status/list, atomic reload, rollback, safe authoring, activate/deactivate, capability binding, and cleanup. Wire it and the resource context into `SkillServices`.

## Task 3: Local `/skills` control plane

Extend the input command types/parser and add `skill_control`. Test list/status/reload/validate/create/activate/deactivate and malformed input. Return structured local results; control-only turns must not invoke the LLM.

## Task 4: Unified resource injection

Extend `ExecutionContext` and preprocessing for `@{workspace://...}` and `@{skill://id/...}`. Test traversal, undeclared Skill resources, budgets, unsupported MCP URIs, and retain `@file` compatibility.

## Task 5: TUI and launcher integration

Add `--skills-root`, `--skill-authoring-root`, `--no-skills`; route local results to TUI; retain active Skill; expose roots/generation/active/bindings. Build and validate the real ncurses UI with a terminal screenshot.

## Task 6: Documentation and regression closure

Document namespaces, trust boundaries, lifecycle, commands, recovery, and authoring. Add an example Skill. Run focused tests, full build/CTest, `git diff --check`, and create focused commits with test evidence.
