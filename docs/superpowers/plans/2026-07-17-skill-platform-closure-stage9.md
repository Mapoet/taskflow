# Skill Platform Closure Stage 9 Implementation Plan

> **For agentic workers:** Execute this plan task-by-task using TDD, deterministic tests, and one reviewable commit per task.

**Goal:** Close the remaining Skill platform contract, runtime wiring, audit, and stress-test gaps so `agent_framework/docs/guides/skill-plan.md` can truthfully declare `platform-complete`.

**Architecture:** Extend the existing typed-resource and pinned-snapshot architecture rather than adding a parallel subsystem. Resource-scoped declarations narrow package policy, all runtime and platform operations emit non-interfering structured audit records, and cache/config/process behavior remains fail-closed and deterministic.

**Tech Stack:** C++20, CMake/CTest, nlohmann::json, yaml-cpp, Linux bwrap/unshare/setrlimit, CycloneDX 1.6, GitHub Actions.

## Global Constraints

- Preserve Manifest v0 compatibility and Manifest v1 fail-closed behavior.
- Never serialize secret values into manifests, diagnostics, events, audit records, CLI output, SBOM, or provenance.
- New task grants, resource permissions, child budgets, and nested runtime context may only narrow parent/package authority.
- Keep package, lockfile, archive, SBOM, and CLI JSON output deterministic.
- Do not declare `platform-complete` until the 1000-cycle leak/stability gate passes.

---

### Task 1: Complete Resource Descriptor Contracts

**Files:**
- Create: `agent_framework/include/agent/skill_permissions.hpp`
- Modify: `agent_framework/include/agent/skill_resource.hpp`
- Modify: `agent_framework/include/agent/skill_manifest.hpp`
- Modify: `agent_framework/src/skills/skill_manifest.cpp`
- Modify: `agent_framework/src/skills/skill_manifest_validate.cpp`
- Modify: `agent_framework/schemas/skill-manifest-v1.schema.json`
- Test: `agent_framework/tests/test_skill_resource_contract.cpp`

- [ ] Move the shared permission declaration to a cycle-free header.
- [ ] Add resource-scoped `permissions` and `depends-on` declarations.
- [ ] Reject resource permissions that exceed manifest permissions and invalid/self/duplicate dependencies.
- [ ] Preserve normalized JSON and legacy compatibility.
- [ ] Run manifest/resource contract tests and commit `feat: complete skill resource descriptor contracts`.

### Task 2: Enforce Declared Cache Policies

**Files:**
- Modify: `agent_framework/include/agent/skill_resource_access.hpp`
- Modify: `agent_framework/src/skills/skill_resource_access.cpp`
- Modify: `agent_framework/include/agent/skill_resource_cache.hpp`
- Modify: `agent_framework/src/skills/skill_resource_cache.cpp`
- Modify: `agent_framework/include/agent/skill_model.hpp`
- Modify: `agent_framework/src/skills/skill_model.cpp`
- Test: `agent_framework/tests/test_skill_resource_access.cpp`
- Test: `agent_framework/tests/test_skill_resource_cache.cpp`
- Test: `agent_framework/tests/test_skill_model.cpp`

- [ ] Make `no-store`, `on-demand`, and `pin` drive observable cache behavior.
- [ ] Keep digest, quota, lease, pin, cancellation, and cleanup fail closed.
- [ ] Test no materialization, on-demand collection, pinned survival, and concurrent acquire.
- [ ] Commit `feat: enforce declared skill cache policies`.

### Task 3: Add Typed Config Resolution

**Files:**
- Create: `agent_framework/include/agent/skill_config.hpp`
- Create: `agent_framework/src/skills/skill_config.cpp`
- Create: `agent_framework/tests/test_skill_config.cpp`
- Modify: `agent_framework/CMakeLists.txt`

- [ ] Resolve a JSON Config resource as defaults plus JSON Merge Patch overrides.
- [ ] Inject JSON-Pointer secret bindings only after manifest/resource/task authorization.
- [ ] Validate the resolved value using the resource input schema.
- [ ] Test defaults, overrides, schema rejection, secret authorization, and redaction.
- [ ] Commit `feat: add typed skill config resolution`.

### Task 4: Unify Runtime Identity and Audit

**Files:**
- Create: `agent_framework/include/agent/skill_audit.hpp`
- Create: `agent_framework/src/skills/skill_audit.cpp`
- Create: `agent_framework/tests/test_skill_audit.cpp`
- Modify: `agent_framework/include/agent/skill_runtime.hpp`
- Modify: `agent_framework/src/skills/skill_runtime.cpp`
- Modify: capability, workflow, resource, cache, lifecycle, package, and registry Skill sources.

- [ ] Add skill/version/package/generation and task/session/trace/depth identity.
- [ ] Apply resource-scoped permissions as a narrowing policy.
- [ ] Emit stable, secret-free audit records for invocation and platform operations.
- [ ] Prove snapshot identity stability, child inheritance, permission denial audit, and non-interfering sinks.
- [ ] Commit `feat: unify skill runtime identity and audit events`.

### Task 5: Enforce CPU, Memory, and Resource Budgets

**Files:**
- Modify: `agent_framework/include/agent/skill_runtime.hpp`
- Modify: `agent_framework/src/skills/skill_runtime.cpp`
- Modify: `agent_framework/src/skills/skill_script_tool.cpp`
- Test: `agent_framework/tests/test_skill_process_sandbox.cpp`
- Test: `agent_framework/tests/test_skill_workflow.cpp`

- [ ] Add resource, output, CPU-time, and address-space limits.
- [ ] Apply Linux `RLIMIT_CPU` and `RLIMIT_AS` before exec.
- [ ] Preserve wall-clock deadline, process-group cleanup, and child budget narrowing.
- [ ] Test busy-loop, memory exhaustion, cancellation, and stable budget diagnostics.
- [ ] Commit `feat: enforce skill process cpu and memory budgets`.

### Task 6: Complete Offline Doctor Checks

**Files:**
- Create: `agent_framework/include/agent/skill_mcp_descriptor.hpp`
- Create: `agent_framework/src/skills/skill_mcp_descriptor.cpp`
- Modify: `agent_framework/src/skills/skill_capability_runtime.cpp`
- Modify: `agent_framework/include/agent/skill_doctor.hpp`
- Modify: `agent_framework/src/skills/skill_doctor.cpp`
- Test: `agent_framework/tests/test_skill_doctor.cpp`

- [ ] Reuse one MCP descriptor parser in runtime and Doctor.
- [ ] Check stdio executables, HTTP origin grants, secret references, all six permission classes, Config dependencies, Model requirements, and cache integrity without opening a live session.
- [ ] Keep diagnostic codes and JSON deterministic and secret-free.
- [ ] Commit `feat: complete offline skill doctor checks`.

### Task 7: Implement `skillctl test --jobs`

**Files:**
- Modify: `agent_framework/src/skills/skill_test_runner.cpp`
- Test: `agent_framework/tests/test_skill_test_runner.cpp`
- Test: `agent_framework/tests/test_skill_cli_contract.cpp`

- [ ] Run cases in a bounded worker pool with a separate jail/Registry/runtime per case.
- [ ] Preserve sorted deterministic output independent of completion order.
- [ ] Stop dispatch on cancellation, join workers, and clean all jails.
- [ ] Test jobs 1/2/8, actual overlap, mixed failures, timeout, and cancellation.
- [ ] Commit `feat: execute skill package tests in parallel`.

### Task 8: Complete SBOM Runtime Metadata

**Files:**
- Modify: `agent_framework/src/skills/skill_sbom.cpp`
- Test: `agent_framework/tests/test_skill_sbom.cpp`
- Test: `agent_framework/tests/test_skill_cli_supply_chain.cpp`

- [ ] Add runtime, source URI, media type, cache policy, model requirements, executable, and optional-dependency metadata.
- [ ] Preserve CycloneDX 1.6 canonical ordering and package reproducibility.
- [ ] Run the SBOM/package/sign/verify chain repeatedly and commit `feat: complete skill sbom runtime metadata`.

### Task 9: Add Stress, Leak, and Scale Gates

**Files:**
- Create: `agent_framework/tests/test_skill_platform_stress.cpp`
- Create: `agent_framework/tests/test_skill_registry_scale.cpp`
- Modify: `agent_framework/CMakeLists.txt`

- [ ] Run 1000 loop/restart/resume/cancel cycles and prove no duplicate side effect.
- [ ] Check process, FD, MCP mock session, cache lease, jail, and snapshot/package lease cleanup.
- [ ] Measure deterministic 1/100/1000/10000 Skill scan/parse/route/publish behavior and emit JSON metrics.
- [ ] Add `skill-platform-stress`, `skill-resource-leak`, and `skill-registry-scale` labels.
- [ ] Commit `test: add skill platform stress and scale gates`.

### Task 10: Close CI and Documentation

**Files:**
- Modify: `.github/workflows/ubuntu.yml`
- Modify: `agent_framework/docs/guides/skill-plan.md`
- Modify: `agent_framework/docs/guides/skill-supply-chain.md`
- Modify: `agent_framework/CMakeLists.txt`

- [ ] Add offline Stage 9 CI gates and installed-artifact checks.
- [ ] Run focused tests, all `skill-*` labels, 20-repeat stability, stress/scale, complete Debug build, and full offline+loopback CTest.
- [ ] Update the stale capability matrix and completion checklist using measured evidence.
- [ ] Run `git diff --check`, verify a clean worktree, and commit `docs: complete stage 9 platform closure`.

