# Skill Workflow Stage 4 Implementation Plan

**Goal:** Add a versioned, policy-controlled Skill Workflow runtime that maps loops and nested workflows to the existing Taskflow-native workflow primitives.

## Scope

- Parse and validate `agent.taskflow/workflow/v1` JSON descriptors.
- Pin the root Skill and exact-version dependency manifests for each run.
- Invoke Skill capabilities through task-scoped bindings without global ToolBus publication.
- Execute tool, nested workflow, loop, and child-task nodes with explicit JSON mappings.
- Propagate cancellation, deadlines, trace/depth/attempt/iteration, permission grants, and byte budgets.
- Define retry, restart, and resume checkpoints plus an idempotency ledger for side effects.
- Normalize local and A2A child-task metadata and terminal results.

## Boundaries

- The DSL has no arbitrary expression evaluator or inline executable code.
- Cross-package references require a dependency declared with an exact version.
- The dependency lock is immutable for one run; persistent lockfiles and content-addressed installation remain Stage 5.
- Checkpoints are caller-owned in-memory values; durable crash recovery remains Stage 5.
- Remote resume is sent as a new A2A task carrying mode and checkpoint metadata. A peer may reject it explicitly.

## Verification

1. Descriptor and mapping unit tests.
2. Scoped capability concurrency tests.
3. Loop cardinality, cancellation, restart, resume, and idempotency tests.
4. Nested workflow and local/remote child protocol integration tests.
5. Existing Skill, Workflow, AgentLoop, Subflow, and A2A regression suites.
6. Full project build and CTest run.

## Commit Gate

Create the Stage 4 commit only after focused tests, integration tests, `git diff --check`, and the full configured CTest suite pass.

## Completion Evidence

- Full project build completed successfully with `cmake --build build -j4`.
- Focused functional and module tests passed for Skill Workflow, Workflow modules/control flow, ChildTask, TaskControl, Agent loop/subflow, and scoped capability bindings.
- All 16 tests carrying the Stage 4 integration labels passed.
- `skill_workflow_stage4` passed 20 consecutive executions with CTest `--repeat until-fail:20`.
- The complete configured suite passed: 3002/3002 tests, 0 failures, in 228.94 seconds.
- `git diff --check` passed. The workspace has no configured sanitizer build, so no sanitizer result is claimed.
