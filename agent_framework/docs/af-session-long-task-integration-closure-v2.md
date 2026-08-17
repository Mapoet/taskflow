# AF Session / Long-task Integration Closure v2 Implementation Plan

> **For agentic workers:** execute AF-SLTR0 through AF-SLTR8 in order. Every task uses checkbox tracking, test-first changes, reviewable commits, and must preserve the user's unrelated working-tree files.

**Goal:** Make Session, Turn, Task, Run, Harness, Invocation, Artifact/Evidence and TaskClosure one durable, observable production system whose Web/TUI/ImGui/CLI behavior supports long-running work without false completion, orphan execution or state divergence.

**Architecture:** Conversation remains the interaction authority, PersistentTask owns work across turns, LongTaskWorkflow owns durable execution, and TaskClosureController alone owns verified completion. A runtime-owned composition wires stores, workers and adapters into all live entry points; a reconciliation service repairs or quarantines incomplete cross-store transitions, while a single revision-aware Operations projection feeds every UI.

**Tech Stack:** C++20, Taskflow/workflow graph, SQLite WAL/FULL stores, optional PostgreSQL control plane, nlohmann/json, httplib/SSE, existing Agent Framework Harness/Run/LongTask/LLM Role Runtime, CMake/CTest.

## Global Constraints

- Model `EndTurn`, Conversation completion, Harness pipeline completion, Invocation terminal and Task verified completion are distinct.
- Only `TaskClosureController` may issue `completed_verified`.
- Empty answer without current-turn artifact/receipt/continuation is never successful delivery.
- Unknown non-idempotent effects enter `ManualReview`; they are never replayed automatically.
- Production must not accept scripted/test callbacks as workflow or execution adapters.
- Every durable mutation uses tenant/conversation/task/run identity, monotonic revision and canonical digest.
- UI reads committed durable projections; it does not derive authority from labels or transient callbacks.
- ProviderLive evidence may remain `NotCertified`; Offline or demo evidence must never replace it.
- Preserve unrelated user changes and generated research files already present in the working tree.

---

## Evidence baseline — 2026-08-16

- `web_ui_demo-harness.sqlite3`: 58 completed, 1 failed, 9 running checkpoints.
- `web_ui_demo-conversation.sqlite3`: 32 completed, 55 failed, 5 running turns.
- The sole durable active Task links 10 turns but remains `active/running` with `plan_revision=0`.
- `build_live_runtime()` leaves `harness_turn_executor`, `long_task_executor` and `task_control_service` unset.
- Non-production long profiles therefore use lightweight Harness compatibility; production has no owned default composition and fails closed.
- CTest registers 275 tests, while the current `build-ui` directory has only a small subset of executables; broad Phase 4 runs contain `Not Run` entries and are not proof of pass.
- Empty-delivery targeted regression is closed, including stream merge, retry, current-turn receipt scoping and internal-code non-disclosure.

## AF-SLTR0 — Truth baseline and orphan reconciliation

**Implementation evidence (2026-08-16):** deterministic dry-run/apply reconciler,
bounded digest-validated Conversation enumeration, restart-safe `AwaitingExternal`,
CAS/tamper/restart tests and `af_state_reconcile` are implemented. The current Web
stores were scanned read-only; no historical effect was replayed or marked complete.

**Files:**

- Create: `include/agent/recovery/system_state_reconciler.hpp`
- Create: `src/recovery/system_state_reconciler.cpp`
- Create: `tests/test_system_state_reconciler.cpp`
- Create: `tools/af_state_reconcile.cpp`
- Modify: `include/agent/conversation/store.hpp`
- Modify: `src/conversation/sqlite_store.cpp`
- Modify: `include/agent/harness/store.hpp`
- Modify: `src/harness/store.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Produce `SystemStateReconciler::scan(const ReconciliationScope&, int64_t now_ms)` returning typed findings without mutation.
- Produce `SystemStateReconciler::apply(const ReconciliationPlan&, ReconciliationPolicy)` using checkpoint CAS and append-only audit events.
- Classify findings as `Recoverable`, `FailTerminal`, `AwaitingExternal`, or `ManualReview` using Invocation/effect evidence.

- [ ] Add failing tests for nine stale Harness execution checkpoints, five zero-iteration Conversation turns, terminal Conversation/running Harness divergence, unknown side effect and idempotent attachable invocation.
- [x] Add bounded store enumeration APIs; validate stored digest before returning records.
- [x] Implement deterministic scan and plan digest; repeated scans of unchanged stores must be byte-identical.
- [x] Implement dry-run default and explicit apply; never infer completed/verified.
- [x] Add restart, stale-CAS and corrupted-checkpoint negative tests.
- [x] Run the reconciler and adjacent Harness/Conversation regressions in `build-ui`.

## AF-SLTR1 — Runtime-owned production composition

**Implementation evidence (2026-08-16):** `ProductionLiveRuntime` owns the validated
Harness runtime, result-view assembler, TaskControl service and deployment lifetime
anchors. Common bootstrap can build and inject its response/long-task executors and
publishes deployment manifests. Complete positive composition/lifetime certification
remains open because the deployment-owned store graph fixture is not yet assembled.

**Files:**

- Create: `include/agent/runtime/production_live_runtime.hpp`
- Create: `src/runtime/production_live_runtime.cpp`
- Create: `tests/test_production_live_runtime.cpp`
- Modify: `examples/common/agent_example_bootstrap.hpp`
- Modify: `src/harness/production_builder.cpp`
- Modify: `include/agent/harness/production_dependencies.hpp`

**Interfaces:**

- Produce `ProductionLiveRuntime::build(const ProductionLiveRuntimeOptions&) -> ProductionRuntimeBuildResult`.
- The returned owner keeps all Stores, workers, adapters and callback captures alive for the full process lifetime.
- Expose typed `harness_executor()`, `long_task_executor()`, `task_control_service()` and readiness manifest.

- [ ] Test that production bootstrap creates all three services or fails with exact missing dependency codes.
- [ ] Test that demo/test profiles are explicit compositions and cannot be mistaken for production-ready.
- [ ] Build ownership graph for Conversation, Task, Run, Harness, Plan, Invocation, IncrementalResult, Effect, Approval, Memory, Assurance, Judge and telemetry stores.
- [x] Wire `DefaultProductionCompositionBuilder` and typed boundary adapters; reject callback-origin production adapters.
- [ ] Inject the owned runtime into all five demos and AgentServer through the common bootstrap.
- [ ] Add destruction/lifetime, startup recovery and timer-worker tests.

## AF-SLTR2 — Persistent task planning

**Implementation evidence (2026-08-16):** `TaskOrchestrator` now owns create/resume,
requirement revision, status/control and new-task switching. The SQLite registry adds
a migration-safe CAS `bind_plan` that upgrades the existing initial RunLink and pins
plan/task-contract digests; this removes the previous duplicate-INSERT dead end.
`TaskPlanningService` now derives a canonical TaskContract from the immutable requirement
chain, runs the multi-stage LLM cognition workflow, persists revisioned Intake, Plan,
TaskContext and typed PlanNode descriptors, then binds the plan through Task CAS. Exact
revision replay is idempotent and does not invoke the LLM again. Requirement amendments
now atomically create their new RunLink; the cognition workflow consumes a canonical
supersession request and CAS-commits the next immutable plan revision. SQLite reopen,
stale concurrent amendment and plan-supersession tests cover the full path.

**Files:**

- Create: `include/agent/conversation/task_orchestrator.hpp`
- Create: `src/conversation/task_orchestrator.cpp`
- Create: `tests/test_task_orchestrator.cpp`
- Modify: `include/agent/conversation/task_registry.hpp`
- Modify: `src/conversation/sqlite_task_registry.cpp`
- Modify: `src/planning/cognition_pipeline.cpp`
- Modify: `examples/common/agent_example_bootstrap.hpp`

**Interfaces:**

- Produce `TaskOrchestrator::open_or_resume(const TurnRequest&, TaskInputIntent)` and `bind_plan(...)`.
- New executable tasks require `requirement_revision>=1`, `plan_revision>=1`, TaskRunLink and pinned plan/task-contract digests before dispatch.

- [x] Test initial, continue, amend, replan, status, cancel, attach, explicit new task and ambiguous active-task resolution.
- [x] Replace inline registry mutation in the bootstrap with the orchestrator transaction boundary.
- [x] Invoke cognition/planning for executable profiles and persist the resulting plan before scheduling.
- [x] Bind descriptor revisions to PlanStore and ProductionWorkflowInputRepository.
- [x] Fail closed in the plan binding boundary on plan revision zero, digest absence,
  requirement drift and stale Task/Run revisions; criteria enforcement remains open.
- [x] Test process restart, concurrent amendment CAS and plan supersession, including
  idempotent replay without a second LLM invocation.

## AF-SLTR3 — Unified terminal coordination

**Early implementation evidence (2026-08-16):** `TaskStateCoordinator` now defines the
versioned correlated-state decision boundary for Conversation, Task, Run, Harness,
Invocation/effect settlement and semantic closure. It explicitly preserves
`execution_completed_unverified`, refuses verified closure while invocations/effects are
unsettled, routes unknown effects to manual review, and does not let an early Conversation
failure terminate a still-running Harness/Run. A FULL-synchronous SQLite command journal now
persists source-event-deduplicated coordination commands before applying Task revision CAS;
restart replay, idempotent reapply and stale-command quarantine are covered. Production Live
publishes this command after correlating Harness, Run, Invocation and effect state. Remaining
work is publication from non-Live terminal producers and the complete participant crash matrix.

**Files:**

- Create: `include/agent/recovery/task_state_coordinator.hpp`
- Create: `src/recovery/task_state_coordinator.cpp`
- Create: `tests/test_task_state_coordinator.cpp`
- Modify: `src/conversation/conversation_engine.cpp`
- Modify: `src/conversation/harness_turn_adapter.cpp`
- Modify: `src/harness/task_closure.cpp`
- Modify: `src/tool_runtime/orphan_recovery.cpp`

**Interfaces:**

- Produce `TaskStateCoordinator::observe(const CorrelatedStateEvent&)` and `reconcile(task_id)`.
- Produce a versioned transition matrix covering Conversation, Harness, Task, Run and Invocation states.

- [ ] Test all terminal/continuation combinations, especially Conversation failure with running Harness and late Invocation completion.
- [ ] Publish coordinator commands from every terminal boundary.
- [x] Close or suspend active-task pointers only through coordinator/TaskRegistry transitions;
  verified closure additionally requires the semantic closure signal.
- [x] Keep pipeline completion named `execution_completed_unverified` in coordinator decisions.
- [ ] Add crash points between every participant prepare/commit and verify restart convergence.
- [ ] Assert orphan-running, false-verified and duplicate-effect rates are zero.

## AF-SLTR4 — Native long-task main path

**Early implementation evidence (2026-08-16):** Conversation now has explicit
`AwaitingInput`, `AwaitingApproval` and `AwaitingExternal` stop reasons mapped to durable
nonterminal Turn phases. Production long-task entry runs TaskPlanningService before the
Harness; clarification and approval can no longer collapse into failed or empty EndTurn.
Production result delivery now assembles invocation partial-result previews into the candidate
answer and fails closed with `execution_completed_without_deliverable` when a completed Harness
has no deliverable, instead of emitting `interactive_execution_empty_delivery` after the fact.

**Files:**

- Create: `include/agent/runtime/task_execution_router.hpp`
- Create: `src/runtime/task_execution_router.cpp`
- Create: `tests/test_native_long_task_path.cpp`
- Modify: `src/conversation/harness_supported_runtime.cpp`
- Modify: `src/tool_runtime/long_task_workflow.cpp`
- Modify: `src/harness/production_composition.cpp`

**Interfaces:**

- Route Conversation/ReadOnlyAnalysis to response Harness and Artifact/Code/External/Professional to owned LongTaskWorkflow.
- Return typed continuation (`AwaitingExternal`, `AwaitingInput`, `AwaitingApproval`) rather than empty EndTurn.

- [x] Test every profile and trust mode, including missing dependency, attempted legacy fallback
  and preservation of typed durable wait reasons.
- [x] Dispatch typed production plan-node descriptors through the executor registry and persist
  invocation watches/timers with idempotent dispatch.
- [x] Feed meaningful invocation evidence to the LLM wait-observe-replan workflow; the fixed
  1,000-heartbeat regression proves routine heartbeat does not invoke the LLM.
- [x] Prevent the production entry point from running Cognition twice: a preplanned Harness
  may begin at PlanApproval only when durable succeeded Intake/Cognition records and immutable
  Intake/Plan/AcceptanceContract pins are supplied; arbitrary stage skipping fails closed.
- [x] Connect Assurance, Remediation, Reverification and Judge Harness outcomes to
  TaskClosure: only an accepted durable report plus a completed Assurance checkpoint,
  independent non-claim evidence, matching oracle method, artifact binding and the
  completion-gated Judge/Operations pins can produce `CompletedVerified`; missing or
  weak evidence remains ManualReview/unverified. `test_production_live_runtime` covers
  both the missing-evidence and verified paths.
- [x] Test artifact-missing, stagnation, budget, approval and external-wait paths.

## AF-SLTR5 — Steering and context endurance

**Early implementation evidence (2026-08-16):** `TaskCommandService` now exposes typed
start/status/output/continue/amend/suspend/cancel/attach/replan commands over the existing
TaskOrchestrator and TaskControl boundaries. Status/output are strictly read-only and tests
prove they do not change Task revision, requirement history or RunLink count. The shared demo
bootstrap now routes status through this control plane before TaskOrchestrator, preventing a
status query from becoming a requirement revision. Five-demo compilation covers the shared
integration. Task planning now persists a revision-bound ContextProjection manifest containing
the immutable TaskContract, approved plan, cognition evidence and task understanding, and binds
its digest plus the typed AcceptanceContract into the durable production task context. Every
subsequent production Harness LLM stage now derives a fresh context digest from the base
projection plus checkpoint revision, approval, artifact, report, Judge, findings and effect
settlement state. `RoleRuntime` binds that digest into the durable input digest and rejects a
production invocation that declares the projection mandatory but omits it.

**Files:**

- Create: `include/agent/conversation/task_command_service.hpp`
- Create: `src/conversation/task_command_service.cpp`
- Create: `tests/test_task_command_service.cpp`
- Modify: `src/conversation/context_projection.cpp`
- Modify: `src/agent/memory_compaction.cpp`
- Modify: `src/tool_runtime/incremental_result_store.cpp`

**Interfaces:**

- Commands: start, status, output, continue, amend, suspend, cancel, attach, replan.
- Project immutable TaskContract, unresolved criteria, current plan, active invocation, approval and artifact/evidence refs into every model invocation.

- [x] Test that status/output are read-only and create no Task requirement or execution RunLink;
  the UI-facing status response may still have its own Conversation control Turn.
- [x] Test steering during model, tool, approval and external wait boundaries.
- [x] Externalize oversized results through IncrementalResultStore/ObjectStore and expose only
  bounded typed chunk/manifest references in the LLM/UI view.
- [x] Compact twice and compare mandatory-state digest before/after; all mandatory projection
  kinds are now non-truncatable rather than a hard-coded contract/policy/citation subset.
- [x] Test disconnect/reconnect, cancellation propagation and unknown-effect reconciliation.

## AF-SLTR6 — Unified Operations and UI

**Files:**

- Modify: `include/agent/ui/phase4_operations.hpp`
- Modify: `src/ui/production_interaction_assembler.cpp`
- Modify: `src/ui/store_backed_operations.cpp`
- Modify: `examples/web_ui_static/app.js`
- Modify: `examples/web_ui_static/style.css`
- Modify: `examples/common/ftxui_console_view.cpp`
- Modify: `examples/common/imgui_console_view.cpp`
- Modify: `src/ui/cli_handler.cpp`
- Create: `tests/test_unified_task_operations.cpp`

**Interfaces:**

- One revisioned `OperationsSnapshot` exposes Session/Task/Run/Turn/Plan/Invocation/criteria/partial/artifact/approval/finding/remediation/closure.
- Four renderers consume the same snapshot digest and action policy.

- [x] Add snapshot parity and out-of-order replay tests.
- [x] Display response, pipeline and verified completion as distinct labels.
- [x] Add task commands with identity/policy authorization; disable impossible actions.
- [x] Verify live observation update, reconnect replay and restart monotonicity.
- [x] Run all four interfaces and capture real Web/TUI/ImGui screenshots.

**Implementation evidence (2026-08-17):** `LiveOperationsProjection` now consumes
the authoritative `TaskStateCoordinator` decision through a runtime-safe observer
bound by Web, TUI, ImGui and CLI. Conversation `model_stop` events update only the
response-delivery axis and can no longer revoke a previously committed
`TaskClosureController` decision. The canonical additive snapshot exposes
`response_delivery_state`, `pipeline_state` and semantic `task_closure_state` as
separate values; all four renderers use that contract. The release-mode test proves
that a late delivered response preserves `CompletedVerified`, restart replay is
monotonic, and stale task/Harness revisions are ignored. Actual runtime evidence:
`docs/assets/ui/sltr6/web-operations.png`,
`docs/assets/ui/sltr6/tui-operations.png`, and
`docs/assets/ui/sltr6/imgui-operations.png`; CLI was executed against the same durable
Web Operations database and rendered the same three axes. TUI content is complete,
but a short terminal may place the top summary above the initial visible scroll
region; this is a presentation refinement, not a state-projection loss.

**Task-command closure evidence (2026-08-17):** `TaskCommandPolicy` requires an
authenticated actor, exact tenant/conversation identity and command-specific
`task:read`/`task:write`/`task:control` scopes. Commands reject stale task revisions and
invalid lifecycle transitions. Shared LiveRuntime is the demo command path; production
has no implicit principal and fails closed. `phase4.operations.v1` persists authoritative
`task_actions` with scope, enabled state, disable reason and expected revision. Web
prepares enabled commands for canonical Conversation submission; TUI/ImGui/CLI render
the same action contract. Visual evidence: `docs/assets/ui/sltr6/web-task-actions.png`.

## AF-SLTR7 — Test infrastructure and certification

**Implementation evidence (2026-08-16):** CMake now exposes `af_phase4_offline_tests`
and `af_sltr_tests`. The shared certification runner discovers the authoritative CTest
set through CTest JSON, builds the 96 executable targets, preserves the two script-only
checks, executes the selected label, and writes timestamped JUnit evidence. The first
complete `phase4-offline` certification passed 98/98 tests; evidence:
`build-ui/agent_framework/certification/af-sltr-phase4-offline-20260816T150834Z.xml`.
The focused `phase4-long-task` certification passed 8/8; evidence:
`build-ui/agent_framework/certification/af-sltr-phase4-long-task-20260816T150532Z.xml`.
After SLTR6 authority-ordering changes, all 98/98 tests passed again in the local
ProcessLive-capable environment; evidence:
`build-ui/agent_framework/certification/af-sltr-phase4-offline-20260816T160800Z.xml`.
The restricted filesystem/network sandbox run passed 96/98 and rejected localhost
listener creation for the two remote-queue tests; both then passed outside that
network sandbox (2/2). This environmental distinction is retained rather than
rewriting the first run as Passed.

**Files:**

- Modify: `CMakeLists.txt`
- Create: `tests/scripts/run_af_sltr_certification.sh`
- Create: `tests/test_af_sltr_golden.cpp`
- Create: `tests/test_af_sltr_recovery.cpp`
- Create: `tests/test_af_sltr_live_report.cpp`

**Interfaces:**

- Produce build targets `af_phase4_offline_tests`, `af_sltr_tests` and a versioned certification report.
- Report cells are `Passed`, `Failed`, or `NotCertified`; missing executable/evidence is never Passed.

- [x] Ensure every `phase4-offline` CTest entry is built or retained as a script-only check
  by the aggregate target, then executed from the same authoritative CTest JSON selection.
- [ ] Run the ten `af-test.md` Golden Tasks, crash matrix and fixed-seed soak.
- [x] Measure orphan, state divergence, duplicate effect, empty completion, first progress and heartbeat metrics.
- [x] Execute ProcessLive Web/reconnect/restart flows.
- [ ] Execute ProviderLive only when real credentials/endpoints exist; otherwise issue blockers with evidence digest.

## AF-SLTR8 — Documentation and completion audit

**Files:**

- Modify: `docs/af-test.md`
- Modify: `docs/af-session-long-task-integration-plan.md`
- Modify: `docs/agent-framework2claude-code.md`
- Modify: `docs/guides/phase-4-status.md`
- Create: `docs/evidence/af-sltr-traceability.md`

- [x] Requery current SQLite stores and record immutable counts/time/revision scope.
- [x] Map every requirement to source, test, runtime evidence and certification cell.
- [x] Remove or correct completion statements contradicted by runtime evidence.
- [x] Record Offline/ProcessLive/ProviderLive separately.
- [x] Run `rg` checks for stale percentages/claims and `git diff --check`.
- [ ] Perform final requirement-by-requirement completion audit; keep Phase/goal open for any missing mandatory evidence.

**Audit evidence (2026-08-17):**
`docs/evidence/af-sltr-traceability.md` binds current SQLite counts and revisions
to database SHA-256 values, maps all ten Golden Tasks, records the 98/98 JUnit,
and separates Offline, localhost/ProcessLive and ProviderLive cells. Historical
running rows were retained as evidence rather than mutated or reclassified.

## Continuous execution gate

For each work package:

1. Add a failing contract/negative test.
2. Confirm the failure represents the intended missing behavior.
3. Implement the smallest complete production behavior, not a test callback substitute.
4. Run targeted tests and restart/crash cases.
5. Build and run the aggregate Phase 4 offline suite; `Not Run` is failure.
6. Inspect diff for unrelated files and unsafe state mutation.
7. For UI changes, use a real binary and real screenshot.
8. Update this plan's checkbox and evidence section before entering the next package.
