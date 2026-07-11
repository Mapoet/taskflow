# Taskflow 4.x Upstream Upgrade Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upgrade the fork from its Taskflow 3.x-derived core to the latest official Taskflow `master`/4.x API while retaining the custom Workflow and Agent Framework and proving compatibility with automated tests.

**Architecture:** Treat official Taskflow as the upstream core and the local `workflow/` and `agent_framework/` trees as downstream consumers. Merge upstream history on an isolated upgrade branch, preserve fork-only components during conflict resolution, then adapt downstream code and CMake targets to the upstream 4.x public API. Validate in layers: Taskflow core tests, Workflow tests/examples, then Agent Framework unit and smoke tests.

**Tech Stack:** C++20, CMake, CTest, Git submodules, official `taskflow/taskflow` upstream.

## Global Constraints

- Base the work on `dev-agent-1.5` and perform all changes on a new upgrade branch.
- Track the exact official upstream commit used by the merge; initial target is `upstream/master` at `46a9c1ab260950dcc1c50dd9ae8b1398929d370f` (2026-07-06).
- Preserve `workflow/`, `agent_framework/`, their documentation, examples, tests, and fork-specific third-party dependencies.
- Do not overwrite or delete fork functionality merely because it is absent upstream.
- Keep the default language level at C++20 because the downstream Workflow and Agent Framework require it.
- Record configure, build, and test commands and any environment-dependent skips.

---

### Task 1: Establish the Reproducible Upgrade Baseline

**Files:**
- Create: `docs/upstream-upgrade.md`
- Modify: `.git/config` (remote metadata only, via `git remote`)

**Interfaces:**
- Consumes: local branch `dev-agent-1.5`, remote `origin`, official repository `https://github.com/taskflow/taskflow.git`
- Produces: branch `upgrade/taskflow-4x`, remote-tracking ref `upstream/master`, documented old/new commit identifiers

- [ ] **Step 1: Confirm the source branch is clean**

Run: `git status --short --branch`
Expected: `## dev-agent-1.5...origin/dev-agent-1.5` with no changed paths.

- [ ] **Step 2: Create the isolated upgrade branch**

Run: `git switch -c upgrade/taskflow-4x`
Expected: `Switched to a new branch 'upgrade/taskflow-4x'`.

- [ ] **Step 3: Record the baseline and target**

Create `docs/upstream-upgrade.md` with the source commit, upstream commit, version transition, preservation policy, and exact verification commands used by Tasks 3-5.

- [ ] **Step 4: Commit the baseline documentation**

Run: `git add docs/upstream-upgrade.md docs/superpowers/plans/2026-07-11-taskflow-4x-upgrade.md && git commit -m "docs: plan Taskflow 4.x upstream upgrade"`
Expected: one documentation commit on `upgrade/taskflow-4x`.

### Task 2: Integrate the Official Taskflow Core

**Files:**
- Modify: `taskflow/**`
- Modify: `unittests/**`
- Modify: `examples/**`
- Modify: `benchmarks/**`
- Modify: `cmake/**`
- Modify: `doxygen/**`
- Modify: `CMakeLists.txt`
- Preserve: `workflow/**`
- Preserve: `agent_framework/**`
- Preserve: `.gitmodules` and fork-only `3rd-party/**` gitlinks

**Interfaces:**
- Consumes: `upstream/master` at the commit recorded in `docs/upstream-upgrade.md`
- Produces: official 4.x Taskflow headers and core tests combined with fork-only build options `TF_BUILD_WORKFLOW` and `TF_BUILD_AGENT_FRAMEWORK`

- [ ] **Step 1: Create a no-commit upstream merge**

Run: `git merge --no-ff --no-commit upstream/master`
Expected: either a staged merge or an explicit conflict list; no fork-only directory is accepted as deleted.

- [ ] **Step 2: Resolve tree and build-system conflicts**

Use upstream versions for Taskflow core implementation and core tests. Retain local versions for `workflow/`, `agent_framework/`, `.gitmodules`, and fork-only submodules. Reapply the two downstream CMake options and corresponding `add_subdirectory` calls to the upstream top-level CMake structure.

- [ ] **Step 3: Verify conflict resolution**

Run: `git diff --check && git status --short`
Expected: no unmerged (`UU`, `AA`, `DD`, `DU`, or `UD`) entries and no whitespace errors.

- [ ] **Step 4: Commit the upstream integration**

Run: `git commit -m "merge: integrate Taskflow 4.x upstream core"`
Expected: a two-parent merge commit retaining the recorded upstream commit as parent two.

### Task 3: Restore Core Build and Test Compatibility

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `unittests/CMakeLists.txt`
- Modify as diagnosed: `taskflow/**`, `examples/**`, `cmake/**`
- Test: `unittests/**`

**Interfaces:**
- Consumes: merged Taskflow 4.x headers and official core tests
- Produces: `Taskflow`/`Taskflow::Taskflow` CMake consumption targets and a passing CPU-only core test suite

- [ ] **Step 1: Configure a core-only build**

Run: `cmake -S . -B build-upgrade-core -DCMAKE_BUILD_TYPE=Debug -DTF_BUILD_TESTS=ON -DTF_BUILD_EXAMPLES=ON -DTF_BUILD_WORKFLOW=OFF -DTF_BUILD_AGENT_FRAMEWORK=OFF -DTF_BUILD_CUDA=OFF`
Expected: configuration succeeds without missing target or source errors.

- [ ] **Step 2: Build the core targets**

Run: `cmake --build build-upgrade-core -j2`
Expected: all CPU Taskflow tests and examples compile.

- [ ] **Step 3: Run core tests**

Run: `ctest --test-dir build-upgrade-core --output-on-failure`
Expected: all discovered CPU tests pass.

- [ ] **Step 4: Fix only reproducible failures and repeat Steps 1-3**

For each compiler or API error, add the smallest compatibility change in the owning header/CMake file and rerun the failing target before the full suite.

- [ ] **Step 5: Commit core compatibility**

Run: `git add CMakeLists.txt cmake taskflow unittests examples && git commit -m "build: adapt fork configuration to Taskflow 4.x"`
Expected: a focused compatibility commit, or no commit if the merge already passes unchanged.

### Task 4: Adapt and Test the Workflow Layer

**Files:**
- Modify as diagnosed: `workflow/CMakeLists.txt`
- Modify as diagnosed: `workflow/include/workflow/**`
- Modify as diagnosed: `workflow/src/**`
- Modify as diagnosed: `workflow/examples/**`
- Test: Workflow CTest targets and examples declared by `workflow/CMakeLists.txt`

**Interfaces:**
- Consumes: Taskflow 4.x public headers and `Taskflow::Taskflow`
- Produces: the existing Workflow public API and build targets compiling against Taskflow 4.x

- [ ] **Step 1: Configure Workflow without Agent Framework**

Run: `cmake -S . -B build-upgrade-workflow -DCMAKE_BUILD_TYPE=Debug -DTF_BUILD_TESTS=ON -DTF_BUILD_EXAMPLES=OFF -DTF_BUILD_WORKFLOW=ON -DTF_BUILD_AGENT_FRAMEWORK=OFF -DTF_BUILD_CUDA=OFF`
Expected: configuration succeeds and Workflow targets are generated.

- [ ] **Step 2: Build Workflow**

Run: `cmake --build build-upgrade-workflow -j2`
Expected: Workflow library, examples, and tests compile; any removed Taskflow API is identified by exact call site.

- [ ] **Step 3: Apply API compatibility fixes**

Replace removed or renamed Taskflow 3.x calls with their Taskflow 4.x public equivalents while preserving Workflow signatures and behavior. Add a focused regression test beside the affected Workflow component for every behavioral fix.

- [ ] **Step 4: Run Workflow tests**

Run: `ctest --test-dir build-upgrade-workflow --output-on-failure`
Expected: core and Workflow tests pass.

- [ ] **Step 5: Commit Workflow adaptation**

Run: `git add workflow && git commit -m "fix: adapt Workflow to Taskflow 4.x"`
Expected: a focused Workflow compatibility commit, or no commit if no adaptation is needed.

### Task 5: Adapt and Test the Agent Framework

**Files:**
- Modify as diagnosed: `agent_framework/CMakeLists.txt`
- Modify as diagnosed: `agent_framework/include/**`
- Modify as diagnosed: `agent_framework/src/**`
- Modify as diagnosed: `agent_framework/tests/**`
- Modify as diagnosed: `agent_framework/examples/**`
- Test: Agent Framework unit and offline smoke targets declared by `agent_framework/CMakeLists.txt`

**Interfaces:**
- Consumes: Taskflow 4.x and the adapted Workflow layer
- Produces: the existing Agent Framework APIs, tools, and offline examples compiling and passing tests

- [ ] **Step 1: Configure the complete CPU build**

Run: `cmake -S . -B build-upgrade-full -DCMAKE_BUILD_TYPE=Debug -DTF_BUILD_TESTS=ON -DTF_BUILD_EXAMPLES=OFF -DTF_BUILD_WORKFLOW=ON -DTF_BUILD_AGENT_FRAMEWORK=ON -DTF_BUILD_CUDA=OFF`
Expected: configuration succeeds with all available local dependencies.

- [ ] **Step 2: Build the complete project**

Run: `cmake --build build-upgrade-full -j2`
Expected: Taskflow, Workflow, Agent Framework libraries, unit tests, and offline examples compile.

- [ ] **Step 3: Apply downstream API fixes**

At each failing call site, migrate to a Taskflow 4.x public API without changing Agent Framework behavior. Add or extend the nearest existing unit test to exercise the affected scheduling, cancellation, observer, or graph behavior.

- [ ] **Step 4: Run offline tests**

Run: `ctest --test-dir build-upgrade-full --output-on-failure`
Expected: all offline tests pass; tests requiring live LLM credentials or external services are explicitly listed as skipped in `docs/upstream-upgrade.md`.

- [ ] **Step 5: Commit Agent Framework adaptation**

Run: `git add agent_framework && git commit -m "fix: adapt Agent Framework to Taskflow 4.x"`
Expected: a focused Agent Framework compatibility commit, or no commit if no adaptation is needed.

### Task 6: Final Upgrade Audit

**Files:**
- Modify: `docs/upstream-upgrade.md`

**Interfaces:**
- Consumes: results from the three build directories and the branch history
- Produces: reproducible upgrade report with exact scope, test totals, limitations, and upstream provenance

- [ ] **Step 1: Run repository integrity checks**

Run: `git diff --check dev-agent-1.5...HEAD && git submodule status`
Expected: no whitespace errors; all retained submodules have recorded commits and no unintended dirty markers.

- [ ] **Step 2: Verify upstream ancestry**

Run: `git merge-base --is-ancestor upstream/master HEAD`
Expected: exit status 0.

- [ ] **Step 3: Record verification evidence**

Update `docs/upstream-upgrade.md` with compiler/CMake versions, exact configure commands, build outcomes, CTest pass/fail/skip totals, known environment constraints, and the final commit list.

- [ ] **Step 4: Commit the audit**

Run: `git add docs/upstream-upgrade.md && git commit -m "docs: record Taskflow 4.x upgrade verification"`
Expected: clean working tree on `upgrade/taskflow-4x`.

