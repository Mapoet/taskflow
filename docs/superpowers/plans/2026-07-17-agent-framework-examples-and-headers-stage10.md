# Agent Framework Examples and Public Headers Stage 10 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring the example applications onto the Stage 1–9 Skill platform and reorganize public headers to mirror the implementation modules without immediately breaking legacy include paths.

**Architecture:** One reusable example bootstrap owns Skill/MCP discovery and structured audit rendering; a dedicated offline `skill_platform_demo` exercises the management plane while the interactive applications share the runtime plane. Canonical public headers move under module directories matching `src`; legacy flat headers become one-release forwarding shims and are verified by separate consumer tests.

**Tech Stack:** C++20, CMake, Taskflow/Workflow, nlohmann/json, CTest, cpp-httplib, ncurses, Dear ImGui.

## Global Constraints

- New canonical includes use `agent/<module>/<header>.hpp` and module names mirror `agent_framework/src`.
- Existing `agent/<header>.hpp` paths remain source- and install-compatible forwarding headers for one release cycle.
- All example smoke tests are deterministic and offline.
- UI changes are validated in the real built applications with screenshots.
- No Stage 1–9 security gate, budget, audit identity, package verification, or isolation behavior may be weakened.

---

### Task 1: Freeze the Public Header and Example Baseline

**Files:**
- Create: `agent_framework/tests/test_public_header_layout.cpp`
- Modify: `agent_framework/CMakeLists.txt`

**Interfaces:**
- Consumes: existing source and installed include trees.
- Produces: `public_header_layout` CTest guard and an explicit canonical/legacy header inventory.

- [ ] Add a compile-time test that includes representative flat headers and their future canonical equivalents.
- [ ] Register it with labels `stage10-header-layout;install-contract`.
- [ ] Build the existing `agent_framework_examples` target to establish the pre-migration baseline.
- [ ] Commit as `test: establish stage 10 public api baseline`.

### Task 2: Add Shared Example Bootstrap

**Files:**
- Create: `agent_framework/examples/common/agent_example_bootstrap.hpp`
- Create: `agent_framework/tests/test_example_bootstrap.cpp`
- Modify: `agent_framework/CMakeLists.txt`

**Interfaces:**
- Produces: `example::BootstrapOptions`, `example::BootstrapResult`, `example::bootstrap_agent_services`, and `example::skill_event_json`.
- Consumes: `SkillServices`, `ToolBus`, `SkillEvent`, Cursor MCP configuration, and environment overrides.

- [ ] Test precedence of explicit options, environment values, and Cursor defaults using temporary fixtures.
- [ ] Implement a header-only bootstrap so all optional example targets share identical runtime behavior without a new installed library.
- [ ] Verify event JSON contains event type, skill/resource/task/run identities, iteration, sequence, and details.
- [ ] Replace duplicated bootstrap helpers in CLI, TUI, Web, and ImGui examples.
- [ ] Commit as `refactor: share example agent bootstrap`.

### Task 3: Add a Complete Offline Skill Platform Reference App

**Files:**
- Create: `agent_framework/examples/skill_platform_demo.cpp`
- Create: `agent_framework/tests/test_skill_platform_demo.cpp`
- Modify: `agent_framework/CMakeLists.txt`

**Interfaces:**
- CLI: `skill_platform_demo <skills-root> [skill-id] [--jobs N]`.
- Output: `agent.taskflow/skill-platform-demo/v1` JSON containing validation, inspection, permissions, doctor, tests, cache verification, resolved configuration, audit events, and supply-chain metadata.

- [ ] Write an offline process-level test using `tests/fixtures/skills/stage6-valid`.
- [ ] Implement safe read-only command-service probes and typed configuration resolution.
- [ ] Exercise runtime begin/finish with a structured audit sink and include stable identities in output.
- [ ] Generate the selected package SBOM/provenance summary without installation or network access.
- [ ] Register `skill_platform_demo_smoke` under `stage10-examples;skill-platform-e2e`.
- [ ] Commit as `feat: add complete skill platform example`.

### Task 4: Modernize Existing Examples

**Files:**
- Modify: `agent_framework/examples/simple_agent.cpp`
- Modify: `agent_framework/examples/multimodal_agent.cpp`
- Modify: `agent_framework/examples/tool_integration.cpp`
- Modify: `agent_framework/examples/workflow_custom.cpp`
- Modify: `agent_framework/examples/cli_agent_demo.cpp`
- Modify: `agent_framework/examples/cli_agent_skills_demo.cpp`
- Modify: `agent_framework/examples/cli_a2a_orchestrator_demo.cpp`
- Modify: `agent_framework/examples/{imgui,tui,web_ui}_agent_demo.cpp`
- Modify: `agent_framework/CMakeLists.txt`

**Interfaces:**
- All examples return non-zero on a failed invariant and support an offline smoke path where external services would otherwise be required.

- [ ] Replace the three placeholder programs with real multimodal preprocessing, ToolBus, and workflow demonstrations.
- [ ] Move `simple_agent` to the current graph/workflow API and deterministic output.
- [ ] Give the A2A orchestrator optional Skill services through the shared bootstrap.
- [ ] Route structured Skill audit output consistently in CLI and rich UI variants.
- [ ] Add CTest smoke cases for all deterministic examples.
- [ ] Run `agent_framework_examples` and the `stage10-examples` label.
- [ ] Commit as `feat: modernize agent framework examples`.

### Task 5: Move Canonical Public Headers into Modules

**Files:**
- Move: `agent_framework/include/agent/*.hpp` into matching module directories.
- Modify: all canonical public headers and implementation includes.
- Create: flat forwarding headers under `agent_framework/include/agent/*.hpp`.

**Interfaces:**
- Canonical examples include `<agent/skills/skill_runtime.hpp>`, `<agent/toolbus/toolbus.hpp>`, and `<agent/graph_executor/graph_executor.hpp>`.
- Legacy examples continue to compile with `<agent/skill_runtime.hpp>`, `<agent/toolbus.hpp>`, and `<agent/graph_executor.hpp>`.

- [ ] Move headers according to the checked-in module map.
- [ ] Change cross-module includes to canonical angle-bracket paths.
- [ ] Generate minimal forwarding shims with `#pragma once` and the canonical include.
- [ ] Compile every canonical header in isolation.
- [ ] Compile representative legacy forwarding headers with deprecation warnings disabled for CI stability.
- [ ] Commit as `refactor: organize public headers by module`.

### Task 6: Update Build, Install, Consumers, and Documentation

**Files:**
- Modify: `agent_framework/CMakeLists.txt`
- Modify: `.github/workflows/ubuntu.yml`
- Create: `agent_framework/docs/guides/public-header-migration.md`
- Modify: `agent_framework/docs/guides/skill-plan.md`
- Modify: repository sources, tests, tools, examples, and docs containing old canonical includes.

**Interfaces:**
- Installed tree contains `agent`, `node`, canonical module headers, and compatibility shims.
- External consumer builds once with canonical paths and once with legacy paths.

- [ ] Replace the manually incomplete header list with a deterministic recursive inventory.
- [ ] Install `include/` rather than only `include/agent`, preserving `node` headers.
- [ ] Add source-tree and installed-tree consumer checks.
- [ ] Document the old-to-new path mapping and one-release removal policy.
- [ ] Add Stage 10 CI build and CTest labels.
- [ ] Commit as `docs: publish modular header migration`.

### Task 7: Full Verification and UI Evidence

**Files:**
- Modify only if verification reveals defects.

**Interfaces:**
- Produces reproducible build, CTest, install, and screenshot evidence.

- [ ] Configure a fresh Debug build with examples and testing enabled.
- [ ] Build the complete project and optional TUI/Web/ImGui applications available in the environment.
- [ ] Run `stage10-examples`, `stage10-header-layout`, all Skill labels, then full CTest.
- [ ] Install into `/tmp/taskflow-stage10-install` and build canonical and legacy external consumers.
- [ ] Launch Web, TUI, and ImGui builds in their real runtime and capture screenshots after readiness.
- [ ] Run `git diff --check`, verify the worktree, and record all evidence in the final Stage 10 commit.
- [ ] Commit as `test: complete stage 10 integration verification`.

## Self-Review

- Spec coverage: examples, Stage 1–9 management/runtime surfaces, header modules, compatibility, install, CI, and real UI validation are each assigned to a task.
- Placeholder scan: the plan intentionally removes the three existing source TODO placeholders and contains no deferred implementation placeholders.
- Type consistency: shared bootstrap types are produced by Task 2 and consumed by Task 4; canonical include paths and compatibility shims are produced by Task 5 and consumed by Task 6.
