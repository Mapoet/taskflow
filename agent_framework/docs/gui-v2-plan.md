# Agent Framework Product GUI v2 Implementation Plan

> **Convergence notice (2026-08-18):** remaining GUI work is executed through
> [`af-task-semantics-gui-v3-plan.md`](af-task-semantics-gui-v3-plan.md), which
> connects task semantics, Decisions, planning, Run supervision, projections and
> the formal workbench as vertical slices. This file remains the GUI capability
> ledger and must be updated as those slices pass their gates.

> **For agentic workers:** Execute vertical slices in order. A visible control is allowed only when a real authorized server action exists.

**Goal:** Upgrade the fixed-default Web demo into a multi-session, multi-user, observable and auditable Agent workbench.

**Architecture:** A Product Session Catalog and unified RuntimeSubject sit above the existing Conversation/Task/Run stores. A per-session Run Supervisor and authorized event gateway expose versioned APIs to a formal React application. Harness, Plan, Memory, Approval, Tool and Evidence remain authoritative backend projections.

**Tech Stack:** C++20, SQLite/PostgreSQL repositories, httplib/websocketpp, TypeScript, React, Vite, TanStack Query, Playwright and CTest.

## Global constraints

- Product Session, Conversation, Task, Run, Turn and runtime checkpoint remain distinct objects.
- No new endpoint or client code may special-case session `default`.
- UI state never determines completion, authorization or side-effect truth.
- Every command is idempotent and revision-aware.
- Every UI batch requires real-browser screenshots at desktop, tablet and mobile widths.

## GUI0 — Identity/lifecycle RFC and capability inventory `[~]`

- [x] Freeze object definitions, state machines, error codes and evidence levels in `gui-identity-lifecycle-rfc.md`.
- [ ] Map every visible control to an action, detail route, configuration route or explicit read-only label.
- [ ] Define Legacy adapter lifetime and prohibit new `/ui/*` features.

## GUI1 — Unified RuntimeSubject `[~]`

**Files:** create `include/agent/identity/runtime_subject.hpp`, adapters and propagation tests.

- [x] Define tenant, organization, principal, project, workspace, session, conversation, task, run, turn, agent and auth revision in one typed subject.
- [x] Reject missing or inconsistent identity at the typed production boundary.
- [x] Retain a typed Legacy adapter only for migrated local data.
- [ ] Propagate the subject through every API, event, tool, memory, approval and artifact adapter.

## GUI2 — Product Session Catalog `[~]`

**Files:** create `session/catalog_types.hpp`, `session/session_catalog.hpp`, SQLite repository, membership repository and list projection.

- [x] Implement create/get/list/search/rename/tag/pin/move repository operations.
- [x] Implement archive/trash/restore and asynchronous purge lifecycle state.
- [x] Implement stable sequence cursor pagination, revision CAS and membership roles.
- [ ] Migrate legacy default data without conflating checkpoint rows.
- [~] Test tenant/membership visibility, cursor stability, reopen recovery and stale CAS; endpoint authorization and concurrent race stress remain.

## GUI3 — Run Supervisor and scheduler `[~]`

- [~] A SQLite durable per-session Run state and command queue now exists; replacing the Web demo's `g_agent_busy` awaits GUI4 endpoint integration.
- [~] Persist typed start/steer/queue/comment/fork/cancel commands with parent Run/Session validation and payload-exact idempotency; executor-side application remains.
- [x] Add tenant-scoped organization/user/project/provider quotas, lease renewal, AwaitingInput lease, epoch fencing and stale-worker rejection.
- [~] Prove three-session parallelism, restart recovery, lease takeover and command idempotency; browser detach and side-effect executor integration remain.

## GUI4 — Versioned API and authorized realtime gateway `[~]`

- [~] Add reusable `/api/v1/sessions`, `/runs`, `/commands`, `/events`, `/artifacts` and `/approvals` modules outside demo code; artifact/approval reads are production-scoped, while approval mutations remain intentionally routed through PDP/AccountableApprovalExecutor.
- [x] Add an authorized SSE fallback with `Last-Event-ID`, retention-floor/head validation and cursor recovery, plus a bounded multi-session event multiplex core and a real websocketpp transport supporting authorized subscribe/update/unsubscribe.
- [x] Reuse the durable ConversationStore event log as the replay fact source, with role-filtered User/Operations/Audit visibility and scan-safe cursors.
- [x] Resolve authenticated RuntimeSubject server-side, enforce tenant/organization/project/workspace membership boundaries, and cover the transport with real loopback HTTP/SSE tests.
- [x] Serve artifact detail only from view-safe Interaction Projection nodes and bind approval detail to Contract identity plus durable Run→Session ownership; never expose arbitrary workspace paths or mutate ApprovalStore directly.
- [x] Initialize the pinned websocketpp submodule at `4dfe1be74e684acca19ac1cf96cce0df9eac2a2d`; CMake compiles the transport only when the real headers exist. The pinned 0.8.2-era headers are isolated to C++17 translation units while the public framework remains C++20.
- [x] Verify a real loopback client against the gateway for authenticated subscribe/update/unsubscribe, replay/cursor delivery, oversized-frame rejection, unauthorized handshake rejection and clean shutdown (`session_run_websocket_api`); sustained slow-consumer/backpressure stress remains production certification work.
- [ ] Bind the versioned API and gateway to the formal Web application composition; the legacy `/ui/*` demo transport is not treated as production parity.

## GUI5 — Capability Manifest `[~]`

- [x] Publish a versioned per-Session manifest with action ID, route, required role, enabled state, reason, expected revision and UI hint.
- [~] Cover Session lifecycle, Run start/steer/queue/comment/fork/cancel, Event replay/stream, artifact view and approval view/decision; composer and Skill/MCP management remain.
- [~] Mark unbound accountable approval mutation explicitly disabled (`accountable_executor_required`); formal Web shell consumption and pseudo-control removal remain GUI6.

## GUI6 — Formal Web shell

- [ ] Create TypeScript/React/Vite application with routed Session browser.
- [ ] Implement responsive left navigation, central workspace and on-demand context drawer.
- [ ] Implement Session search/create/open/archive/trash/restore and stable URLs.
- [ ] Preserve a thin legacy adapter until production parity.

## GUI7 — Harness and observability integration

- [ ] Drive Run Strip from authoritative Run and Closure state.
- [ ] Drive Activity, Plan, Files, Agent/Skill, Memory, Approval and Evidence drawers from typed projections.
- [ ] Make Observation Snapshot update from the same run-scoped event stream.
- [ ] Link every answer to Run, receipts, artifacts and evidence.

## GUI8–GUI11 — Security, governance, collaboration and management

- [ ] Integrate OIDC/trusted BFF, hierarchical RBAC and audit.
- [ ] Implement Policy and Preference inheritance with immutable Run snapshots.
- [ ] Expose credential status without returning secrets and explain memory selection.
- [ ] Implement presence, conflict recovery, collaborative commands and approval revisions.
- [ ] Build Agent/Skill/Workflow version, test, publish, pin and rollback center.

## GUI12 — Production certification and legacy closure

- [ ] Validate SQLite local and PostgreSQL service profiles.
- [ ] Execute failure injection for database, disk, provider, MCP, runner, receipt and reconnect paths.
- [ ] Validate Chromium, Firefox and WebKit, keyboard use, WCAG 2.2 AA and bilingual content.
- [ ] Remove `g_agent_busy`, fixed default session writes and legacy write endpoints after observation period.

## Acceptance

- One user can run three Sessions concurrently without message, memory, tool or artifact cross-talk.
- Two organizations cannot observe or mutate each other's objects or streams.
- The main workspace contains no fake controls and no permanent empty inspector.
- Completion always comes from Closure/Evidence, never model text.
- Every UI phase includes real runtime screenshots and replay/restart evidence.
