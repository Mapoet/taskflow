# AF Task Semantics and GUI v3 Convergence Implementation Plan

> **For agentic workers:** Execute AF-TGUI0 through AF-TGUI12 in order. Each batch must pass its focused tests and preserve the evidence boundary before the next batch starts.

**Goal:** Connect task understanding, durable decisions, planning, execution, verification, Session management and the user interface into one revision-aware production workflow.

**Architecture:** Task semantics v4 separates user intent, work shape, effect risk and assurance depth. A general durable Decision workflow replaces profile-only clarification. Product Session and Run Supervisor feed a common runtime composition and one durable event/projection plane consumed by the versioned API and formal Web workbench.

**Tech Stack:** C++20, nlohmann/json, SQLite/PostgreSQL repositories, httplib, websocketpp, TypeScript, React, Vite, TanStack Query, Playwright and CTest.

## Global constraints

- Classification, planning and UI never grant execution authority.
- Product Session, Conversation, Task, Run, Turn, Decision, Plan and Operation remain distinct identities.
- A model response, Turn terminal state or UI state never establishes verified task completion.
- Every mutation is authenticated, idempotent and revision-aware.
- Explicit control commands bypass pending semantic decisions and remain available at all wait states.
- Complex read-only work may require a durable Plan; simple write work does not automatically require a long workflow.
- No new endpoint or client special-cases `default`.
- UI changes require real screenshots from the running application at desktop, tablet and mobile widths.
- Evidence is reported separately as Source, Build, Offline, Runtime, ProviderLive and Production.

## AF-TGUI0 — Baseline and traceability

- [x] Audit `af-term-v2-plan.md` and `gui-v2-plan.md` against current source and focused CTest evidence.
- [x] Record the replacement architecture and ordered implementation batches in this document.
- [x] Create `af-task-semantics-traceability.md` with authoritative evidence and current blockers.
- [x] Update legacy plan status after each batch without promoting offline evidence to Live or Production.

## AF-TGUI1 — Task semantics v4

**Files:**

- Create `include/agent/conversation/task_semantics.hpp`.
- Modify `include/agent/conversation/task_classifier.hpp`.
- Modify `src/conversation/task_classifier.cpp`.
- Modify `tests/test_task_classifier.cpp` and the locked evaluation corpus.

**Produces:** independent `WorkShape`, `AssuranceTier`, `TaskSemanticDecision` and a compatibility projection to `TaskExecutionProfile`.

- [x] Parse and validate intent, work shape, effect class, assurance tier, confidence and linguistic evidence independently.
- [x] Preserve v3 input as an explicit migration adapter; new invocations use schema v4.
- [x] Prove that deep read-only research can require planning and that a bounded code edit is not automatically long-running.
- [x] Keep deterministic fallback conservative and non-authoritative.

## AF-TGUI2 — General durable Decision workflow

**Files:**

- Create `include/agent/decision/decision_types.hpp` and `decision_store.hpp`.
- Create `src/decision/sqlite_decision_store.cpp` and `decision_coordinator.cpp`.
- Adapt `task_profile_clarification.*` for migration-only reads.
- Add `tests/test_decision_store.cpp` and `test_decision_coordinator.cpp`.

**Produces:** versioned decision questions, typed options, semantic patches, CAS answer/cancel/expire and restart recovery.

- [x] Support task ambiguity, planning clarification, memory conflict, recovery and run-routing decisions.
- [x] Validate option patches server-side; never execute arbitrary client JSON.
- [x] Make `/status`, `/cancel`, `/stop` and equivalent typed control commands bypass a pending Decision.
- [x] Preserve idempotent replay and reject stale or conflicting answers.
- [x] Migrate existing pending profile clarifications without recreating a Task or Run.

## AF-TGUI3 — Conversation-to-Task promotion

- [x] Add `TaskPromotionPolicy` for direct Turn, bounded Task, long-running Task and continuous Task.
- [x] Keep simple conversational turns lightweight while preserving correlation.
- [x] Promote a Turn to a durable Task when planning, artifacts, effects, approvals or professional assurance are required.
- [x] Preserve prior context when a conversational answer is promoted into an artifact or code task.
- [x] Define completed-task/new-input behavior and multiple Tasks per Product Session.

## AF-TGUI4 — Planning policy and revision binding

- [x] Add a typed `PlanningDecision` with required flag, depth, reasons and policy revision.
- [x] Trigger planning from work shape, dependency count, user request, effect verification and assurance tier rather than profile alone.
- [x] Emit and persist `planning_required` or `planning_skipped` with machine-readable reasons.
- [x] Bind every Plan to requirement revision and mark prior Plans superseded after amendments.
- [x] Use the general Decision workflow for cognition clarification.

## AF-TGUI5 — LLM workflow and observability

- [x] Pin provider, model, prompt, memory view, input/output digest, schema and deployment revision for semantic, cognition, planner, verifier, judge and remediation invocations.
- [x] Emit lifecycle events without raw sensitive prompts.
- [x] Record latency, token use, cost, confidence, fallback and decision outcome.
- [x] Expose calibration and unnecessary-clarification metrics.
- [ ] Validate malformed output, timeout, cancellation and restart behavior.

## AF-TGUI6 — Unified execution snapshot

- [x] Add `TaskExecutionSnapshot` containing task, requirement, plan, run and projection revisions plus authoritative digests.
- [x] Require expected revisions and idempotency keys for mutating commands.
- [x] Return current snapshot, changed fields and safe-retry policy on conflict.
- [x] Display, but never infer, the snapshot in UI projections.

## AF-TGUI7 — Run Supervisor execution integration

- [x] Add `SessionRunWorker`, command dispatcher, lease heartbeat and recovery coordinator.
- [ ] Execute claimed Runs through the production runtime composition.
- [ ] Apply start, steer, queue, comment, fork, cancel, retry, reconcile and escalation commands durably.
- [x] Ensure waiting Decisions and Approvals release execution capacity.
- [ ] Replace `g_agent_busy` and process-global active control with Session/Run ownership.
- [ ] Prove three concurrent Sessions, browser detach, restart takeover and stale fencing rejection.

Implemented boundary: `ProductionLiveRuntime::session_run_executor()` is now the sole
typed bridge from a leased Session Run to the production long-task Harness. It rejects
missing Session/Conversation/Task/Turn bindings and stale leases. Deployment wiring and
positive ProviderLive execution remain required before the execution item may be checked.

## AF-TGUI8 — Canonical runtime events and projections

- [x] Publish semantic, Decision, Task, Plan, Run, Effect, Artifact, Assurance and Closure events through one envelope.
- [x] Build rebuildable Session, Task, Plan, Observation, Activity, Approval, Evidence and Closure projections.
- [x] Drive Conversation and Observation Snapshot from the same run-scoped cursor.
- [x] Detect duplicate, reordered, expired-cursor and schema-incompatible events.

## AF-TGUI9 — Complete versioned API

- [x] Complete Session mutation, membership, restore, purge, data and fork endpoints.
- [ ] Add Task semantics, Decision, Plan, Run snapshot, Observation and Evidence endpoints.
- [x] Route approval mutations only through PDP and `AccountableApprovalExecutor`.
- [x] Publish OpenAPI 3.1 and versioned schemas.
- [ ] Make `/ui/*` a read-compatible legacy adapter with no new feature authority.

## AF-TGUI10 — Formal Session and Task workbench

- [x] Create the TypeScript/React/Vite application under `agent_framework/web`.
- [x] Implement routed Session browser, central conversation, Run Strip and on-demand drawers.
- [x] Render Understanding, Decisions, Plan, Activity, Memory, Files, Approval and Evidence from typed projections.
- [x] Consume Capability Manifest for every visible mutating action and remove pseudo-controls.
- [x] Show revision conflict, stale projection, reconnect and decision states accessibly.
- [x] Preserve a thin legacy static client until runtime parity is demonstrated.

## AF-TGUI11 — System evaluation

- [x] Extend the multilingual corpus with clear conversation, deep read-only research, bounded code, external effect, professional assurance and active-task increments.
- [ ] Measure false high-effect routing, missed planning, unnecessary planning, unnecessary clarification and decision abandonment.
- [ ] Run unit, repository, API, concurrency, restart, event replay, projection rebuild, security and browser E2E suites.
- [ ] Verify no cross-Session message, memory, tool, artifact or event leakage. Event/projection isolation is proved; memory, tool and artifact end-to-end isolation remains.

## AF-TGUI12 — Live certification and legacy closure

- [ ] Execute ProviderLive semantic/planning cases and real MCP/tool recovery cases.
- [ ] Validate SQLite local and PostgreSQL service profiles without claiming an unexecuted multi-node topology.
- [ ] Execute crash, busy, disk, schema, provider, MCP, receipt and reconnect fault matrices.
- [ ] Validate Chromium, Firefox and WebKit plus keyboard, WCAG 2.2 AA and bilingual content.
- [ ] Capture real desktop, tablet and mobile screenshots.
- [ ] Remove fixed default writes, `g_agent_busy` and legacy write endpoints after parity observation.

## Completion gates

- Clear inputs do not produce unnecessary Decisions.
- Material ambiguity produces one durable, relevant Decision with 2–5 choices.
- Complex read-only tasks receive a Plan when policy requires it.
- Control commands remain available during every wait state.
- One user can run three Sessions concurrently with zero cross-talk.
- Every answer links to its Task, Run, Plan, receipts, artifacts and Closure evidence when those objects exist.
- Observation and Conversation converge on the same event head and projection revision.
- Completion is authorized only by TaskClosureController from durable facts.
- Production claims require real Live evidence; skipped or unavailable cells remain explicit blockers.
