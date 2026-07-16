# Remove Flat Agent Headers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove every flat Agent Framework compatibility forwarding header and make module-qualified public includes the only supported API.

**Architecture:** Canonical headers remain under directories that mirror `agent_framework/src`; `agent/a2a`, `agent/internal`, and `node` keep their existing module layouts. CMake, tests, CI, installation checks, and documentation will reject or stop advertising flat `agent/*.hpp` entry points.

**Tech Stack:** C++20, CMake, CTest, GitHub Actions, Markdown.

## Global Constraints

- Delete all 68 three-line compatibility forwarding headers under `agent_framework/include/agent`.
- Do not delete canonical module headers, `agent/a2a`, `agent/internal`, or `node` headers.
- Do not retain a deprecated alias or generated compatibility layer.
- Keep the installed public API canonical-only.
- Run the complete existing regression suite before commit.
- Do not push the resulting commit unless separately requested.

---

### Task 1: Enforce the canonical-only layout

**Files:**
- Modify: `agent_framework/CMakeLists.txt`
- Modify: `agent_framework/tests/test_public_header_layout.cpp`
- Modify: `.github/workflows/ubuntu.yml`

**Interfaces:**
- Consumes: canonical headers under `include/agent/<module>/` and `include/node/`.
- Produces: a configure-time invariant that `include/agent/*.hpp` is empty and a canonical-only compile/install contract.

- [ ] **Step 1: Add a configure-time flat-header rejection gate**

Use a non-recursive glob over `include/agent/*.hpp` and stop configuration with a diagnostic listing any unexpected flat headers.

- [ ] **Step 2: Remove legacy includes from the public-header test**

Keep representative canonical imports for core, agent, graph executor, skills, toolbus, UI, and node modules. Change the success message to `canonical-public-headers-ok`.

- [ ] **Step 3: Change CI installation assertions**

Require canonical Skill, core, and node paths; explicitly fail if representative flat paths such as `agent/skill_config.hpp` or `agent/types.hpp` exist.

- [ ] **Step 4: Configure and build the layout test**

Run `cmake -S . -B build-stage8`, build `test_public_header_layout`, and expect configuration and compilation to succeed after Task 2.

### Task 2: Remove compatibility forwarding headers

**Files:**
- Delete: all files matching `^// Compatibility forwarding header;` below `agent_framework/include/agent`

**Interfaces:**
- Consumes: the canonical target path recorded by each forwarding header.
- Produces: a public include tree with no direct `agent/*.hpp` headers and no `agent/tui/tui_handler.hpp` alias.

- [ ] **Step 1: Record and count the deletion set**

Run `rg -l '^// Compatibility forwarding header;' agent_framework/include/agent -g '*.hpp'`; expected count: `68`.

- [ ] **Step 2: Delete exactly the recorded files**

Use an explicit patch so canonical module headers and non-forwarding `a2a`/`internal` headers cannot be removed accidentally.

- [ ] **Step 3: Verify the layout**

Require both searches to return no files:

```bash
rg -l '^// Compatibility forwarding header;' agent_framework/include/agent -g '*.hpp'
rg --files agent_framework/include/agent -g '*.hpp' -g '!*/**/*.hpp'
```

### Task 3: Migrate documentation to canonical paths

**Files:**
- Modify: `agent_framework/docs/guides/public-header-migration.md`
- Modify: every document containing a removed `include/agent/<name>.hpp` path
- Modify: `agent_framework/docs/guides/skill-plan.md`

**Interfaces:**
- Consumes: the forwarding-header source-to-canonical mapping captured before deletion.
- Produces: valid Markdown links and examples that exclusively reference module-qualified headers.

- [ ] **Step 1: Rewrite old paths using the exact forwarding map**

Replace both source paths and include examples, including relative Markdown targets, with their canonical module paths.

- [ ] **Step 2: Document the intentional breaking change**

State that the compatibility layer has been removed in Stage 10 follow-up work and downstream projects must migrate before updating.

- [ ] **Step 3: Remove the obsolete one-release compatibility claim**

Update `skill-plan.md`, CMake comments, tests, and CI language so no component promises legacy header support.

- [ ] **Step 4: Search for stale references**

Generate the removed basename set from the recorded map and require no old include directive or documentation path to remain.

### Task 4: Verify build and installation contracts

**Files:**
- Test: `agent_framework/tests/test_public_header_layout.cpp`
- Verify: `/tmp/taskflow-stage10-no-flat-install`

**Interfaces:**
- Consumes: canonical-only source and install trees.
- Produces: reproducible evidence that source consumers compile and removed paths are absent.

- [ ] **Step 1: Compile every public header in isolation**

Compile all headers under `include/agent` and `include/node` with C++20 `-fsyntax-only`; expected: zero failures.

- [ ] **Step 2: Build the complete repository**

Run `cmake --build build-stage8 -j8`; expected: exit code `0`.

- [ ] **Step 3: Run the complete regression suite**

Run `env -u DEEPSEEK_API_KEY AGENT_TEST_OFFLINE=1 ctest --test-dir build-stage8 --output-on-failure -j8`; expected: all registered tests pass.

- [ ] **Step 4: Verify installation**

Install to `/tmp/taskflow-stage10-no-flat-install`, compile canonical core/Skill/node consumers, and assert representative removed flat paths do not exist.

- [ ] **Step 5: Run repository hygiene checks**

Run `git diff --check`, inspect `git status --short`, and ensure no generated runtime files are present.

### Task 5: Commit the canonical-only public API

**Files:**
- Commit: all approved Stage 10 follow-up changes

**Interfaces:**
- Consumes: verified changes from Tasks 1–4.
- Produces: one reviewable commit on `upgrade/taskflow-4x`.

- [ ] **Step 1: Stage the approved paths**

Stage `.github/workflows/ubuntu.yml`, `agent_framework`, and this plan document only.

- [ ] **Step 2: Re-run the staged whitespace check**

Run `git diff --cached --check`; expected: no output and exit code `0`.

- [ ] **Step 3: Commit**

```bash
git commit -m "refactor: remove flat agent header aliases"
```

- [ ] **Step 4: Audit final state**

Require a clean worktree and report the commit hash, test count, installation result, and remote-ahead count. Do not push.
