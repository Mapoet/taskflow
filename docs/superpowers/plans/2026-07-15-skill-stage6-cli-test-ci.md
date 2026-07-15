# Skill Stage 6 CLI, Test Runner, and CI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (\`- [ ]\`) syntax for tracking.

**Goal:** Deliver an installable, offline-capable skillctl with stable JSON and exit contracts, package-contained tests, lifecycle commands, package preflight, and dedicated CI gates.

**Architecture:** Stage 6 is split into 6A CLI contracts, 6B lint/test/doctor services, and 6C lifecycle/package/CI. The executable only parses arguments and serializes responses; reusable logic lives in agent_framework. Stage 6 computes package identity but leaves archive, signing, and remote Registry formats to Stage 8.

**Tech Stack:** C++20, CMake/CTest, nlohmann/json, current Skill Registry/Loader/Runtime/Workflow/Lifecycle APIs, OpenSSL SHA-256, GitHub Actions.

## Global Constraints

- Build and install skillctl with AGENT_BUILD_EXAMPLES=OFF.
- Add no required third-party dependency and start no Agent Server.
- Default output is agent.taskflow/skillctl-output/v1 JSON; only read --raw emits raw bytes.
- Preserve skillctl ROOT COMMAND while adding --root, --store, and --format.
- Fixed exits: 0 success, 2 contract/lint/test, 3 not found, 4 operation/dependency, 5 integrity, 6 runtime missing, 64 usage, 70 internal.
- Tests deny network, environment, and secrets and use a temporary jail with explicit mocks.
- Failed gates leave store, skills.lock, history, and Registry generation unchanged.
- package validates, tests, and computes identity; it does not create the Stage 8 archive.
- Use failing test, minimal implementation, regression, and one reviewable commit per task.

---

## File Map

Create:

- agent_framework/include/agent/skill_command.hpp and src/skills/skill_command.cpp.
- agent_framework/include/agent/skill_doctor.hpp and src/skills/skill_doctor.cpp.
- agent_framework/include/agent/skill_test_runner.hpp and src/skills/skill_test_runner.cpp.
- agent_framework/include/agent/skill_package_gate.hpp and src/skills/skill_package_gate.cpp.
- agent_framework/tools/skillctl.cpp.
- agent_framework/schemas/skill-test-v1.schema.json and skillctl-output-v1.schema.json.
- agent_framework/tests/test_skill_cli_contract.cpp, test_skill_doctor.cpp, test_skill_test_runner.cpp, test_skill_package_gate.cpp, and test_skill_cli_lifecycle.cpp.
- agent_framework/tests/fixtures/skills/stage6-valid and stage6-invalid.

Modify agent_framework/CMakeLists.txt, lifecycle header/source, Skill guides, and .github/workflows/ubuntu.yml. Delete agent_framework/examples/skillctl.cpp only after migration.

---

### Task 1: Freeze CLI Response Contracts

**Files:** Create skill_command.hpp/cpp, output schema, CLI contract test; modify CMakeLists.txt.

**Interfaces:** Produce SkillCliExit and SkillCommandResponse::to_json for every later task.

- [ ] Write a failing test requiring apiVersion, command, ok, data, diagnostics, error and NotFound exit 3.
- [ ] Run the test; expect missing skill_command.hpp.
- [ ] Add this interface:

~~~cpp
enum class SkillCliExit : int {
  Success=0, ContractFailed=2, NotFound=3, OperationFailed=4,
  IntegrityFailed=5, DependencyUnavailable=6, Usage=64, Internal=70
};
struct SkillCommandResponse {
  SkillCliExit exit = SkillCliExit::Success;
  std::string command;
  nlohmann::json data = nlohmann::json::object();
  std::vector<SkillDiagnostic> diagnostics;
  nlohmann::json error = nullptr;
  bool ok() const noexcept { return exit == SkillCliExit::Success; }
  nlohmann::json to_json() const;
};
~~~

- [ ] Implement deterministic serialization and an envelope schema with additionalProperties false.
- [ ] Run ctest -R '^skill_cli_contract$'; expect pass.
- [ ] Commit: feat: define stable skillctl contracts.

### Task 2: Promote skillctl to an Installed Target

**Files:** Create tools/skillctl.cpp; delete examples/skillctl.cpp; modify CMake and CLI test.

**Interfaces:** Produce new and legacy argument parsing and production target.

- [ ] Add failing parser tests for positional ROOT and --root ROOT forms.
- [ ] Parse without CLI11; malformed flags return exit 64 and skillctl_usage_error.
- [ ] Add an unconditional skillctl target linked to agent_framework and install it to CMAKE_INSTALL_BINDIR.
- [ ] Configure AGENT_BUILD_EXAMPLES=OFF, build, install to /tmp/taskflow-stage6-install, and run --help.
- [ ] Commit: feat: promote skillctl to installed cli.

### Task 3: Migrate Existing Read-Only Commands

**Files:** Modify command header/source, tool entry, CLI contract test.

**Interfaces:** Produce SkillCommandService::list, show, validate, inspect, read.

- [ ] Add failing tests for sorted list, not-found, invalid Registry, resolved manifest, bounded read, binary encoding, and raw read.
- [ ] Implement the service with shared Registry and Loader.
- [ ] Default read returns mediaType, bytes, encoding, content; --raw returns bytes only.
- [ ] Run CLI, manifest, and Registry tests.
- [ ] Commit: feat: migrate skill inspection commands.

### Task 4: Add Lint, Graph, and Permissions

**Files:** Modify command header/source, tool entry, CLI contract test.

**Interfaces:** Consume manifests, SemVer, grants, snapshot; produce lint, graph, permissions.

- [ ] Add failing lint cases for missing license/authors/media type/runtime/schema/digest, unbounded dependency, and unused permission.
- [ ] Implement warnings-as-errors while warnings remain successful by default.
- [ ] Add graph tests for sorted roots/packages/edges/ranges/digests.
- [ ] Add permission tests for declared/granted/effective/denied and secret redaction.
- [ ] Run skill_cli_contract.
- [ ] Commit: feat: add skill lint graph and permission reports.

### Task 5: Implement Offline Doctor

**Files:** Create doctor header/source/test; modify command and CMake files.

**Interfaces:** Produce SkillDoctor::inspect and doctor command.

- [ ] Add failing cases for missing runtime/CLI/model, malformed MCP, digest mismatch, insufficient filesystem grant, and secret redaction.
- [ ] Check readiness without connecting MCP, loading models, binding ports, or HTTP.
- [ ] Label the test skill-unit and skill-security-negative.
- [ ] Run skill_doctor_offline without network permission.
- [ ] Commit: feat: add offline skill doctor.

### Task 6: Define Skill Test v1

**Files:** Create runner header/source, test schema, runner test, valid/invalid fixtures; modify CMake.

**Interfaces:** Produce strict descriptor parsing, SkillTestRunOptions, SkillTestCaseResult, SkillTestSuiteResult.

- [ ] Add this valid descriptor:

~~~json
{
  "apiVersion":"agent.taskflow/skill-test/v1",
  "kind":"SkillTest",
  "name":"workflow returns normalized value",
  "target":{"kind":"workflow","resource":"main"},
  "input":{"value":7},
  "mocks":{"tools":{"base_value":{"output":{"value":7}}}},
  "expect":{"ok":true,"output":{"value":7},
    "events":[{"type":"invocation_started"},{"type":"invocation_completed"}]}
}
~~~

- [ ] Add invalid version/kind/path/target/size/mock/expect fixtures.
- [ ] Implement strict parsing with skill_test_descriptor_invalid and JSON Pointer locations.
- [ ] Run parser tests; expect all hostile fixtures rejected.
- [ ] Commit: feat: define skill test v1 contracts.

### Task 7: Execute Tests in an Isolated Jail

**Files:** Modify runner header/source, tests, and fixtures.

**Interfaces:** Consume runtimes, TaskControl, script tool, mock ToolBus; produce SkillTestRunner::run.

- [ ] Add resource/tool/workflow/script/CLI cases and digest, timeout, output, traversal, undeclared capability, and secret-negative cases.
- [ ] Pin a snapshot, copy package to a temporary jail, create a fresh Registry, empty env, no secrets, deadline, and declared mocks only.
- [ ] Compare ok, output, error.code, ordered event subsequence, stdout/stderr, exitCode, resourceDigests.
- [ ] Clean processes and jail on success, failure, timeout, and cancel.
- [ ] Repeat the runner test 20 times.
- [ ] Commit: feat: add isolated skill test runner.

### Task 8: Expose the test Command

**Files:** Modify command service, tool entry, CLI contracts.

**Interfaces:** Consume SkillTestRunner::run; produce test [id] [--filter NAME] [--jobs N].

- [ ] Add failing output/exit tests requiring passed, failed, cases, skill_tests_failed exit 2.
- [ ] Reject unmatched filters and jobs outside 1..64.
- [ ] Implement deterministic summaries without environment or secret values.
- [ ] Run CLI and runner tests.
- [ ] Commit: feat: expose package tests through skillctl.

### Task 9: Expose Stage 5 Lifecycle Commands

**Files:** Modify command/tool files; create test_skill_cli_lifecycle.cpp; modify CMake.

**Interfaces:** Consume LifecycleManager operations; produce install/update/enable/disable/remove/rollback requiring --store.

- [ ] Add failing install, enable range, update, rollback, disable, in-use remove, released remove, bad digest, and conflict cases.
- [ ] Map conflict/in-use to 4, integrity/source/lock to 5, missing to 3, missing --store to 64.
- [ ] Return roots, rootRanges, packages, digests, and Registry generation.
- [ ] Run CLI lifecycle and Stage 5 lifecycle tests.
- [ ] Commit: feat: expose skill lifecycle commands.

### Task 10: Add Package Inspection and Preflight

**Files:** Create package gate header/source/test; modify lifecycle, command, tool, and CMake files.

**Interfaces:** Consume validate, lint, runner, Stage 5 SHA-256; produce SkillPackageGate::inspect, package, gated install/update.

- [ ] Add a failing test requiring two inspections to return equal digests without Registry or lock mutation.
- [ ] Refactor package identity into a non-mutating API returning ID, SemVer, manifest, package digest, resource digests.
- [ ] Reject links, special files, traversal, missing resources, and mismatch before writes.
- [ ] Gate in order: validate, lint, test, digest pass one, digest pass two; call LifecycleManager only after success.
- [ ] Compare store entries, lock/history bytes, and Registry generation around every failed phase.
- [ ] Run package gate, CLI lifecycle, and Stage 5 lifecycle tests.
- [ ] Commit: feat: enforce skill package quality gates.

### Task 11: Add Offline CI Gates

**Files:** Modify CMakeLists.txt and .github/workflows/ubuntu.yml.

**Interfaces:** Produce skill-cli-contract, skill-test-runner, skill-package-reproducibility, skill-security-negative labels.

- [ ] Assign exact labels to CLI, doctor, runner, gate, and lifecycle tests.
- [ ] Add a job using TF_BUILD_AGENT_FRAMEWORK=ON, TF_BUILD_TESTS=ON, AGENT_BUILD_EXAMPLES=OFF.
- [ ] Build skillctl and Stage 1–6 test targets only.
- [ ] Run the union of Skill labels without credentials or network variables.
- [ ] Reproduce in build-stage6-ci; require all selected tests pass.
- [ ] Commit: ci: add stage 6 skill quality gates.

### Task 12: Documentation and Final Verification

**Files:** Modify skills.md, skill-plan.md, and CMake install rules.

**Interfaces:** Produce operator reference, installed schemas, completion evidence.

- [ ] Document commands, envelope, exits, Test v1, package gate, lifecycle examples, and Stage 8 boundary.
- [ ] Install manifest, test, and output schemas under the Agent Framework data schema directory.
- [ ] Run git diff --check and complete build.
- [ ] Run all Skill labels and focused Stage 6 tests 20 times.
- [ ] Install under /tmp/taskflow-stage6-install and validate fixtures with installed skillctl.
- [ ] Run complete available CTest and report sandbox-blocked HTTP/A2A/online-model tests separately.
- [ ] Record measured evidence.
- [ ] Commit: docs: complete stage 6 skill operations.

---

## Exit Criteria

- Production skillctl builds and installs with examples disabled.
- Every non-raw command emits stable JSON and documented exits.
- Lint, test, doctor need no Agent Server, port, or network.
- Tests deny network, environment, and secrets.
- Failed tests block package/install before persistent mutation.
- Identical content produces identical package/resource digests.
- Lifecycle CLI preserves conflict details, lock identities, and generations.
- Stage 6 gates run in CI.
- Stage 1–6 Skill labels pass and focused tests pass 20 times.

