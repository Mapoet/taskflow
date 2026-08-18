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
- [x] Validate malformed output, timeout, cancellation and restart behavior.

## AF-TGUI6 — Unified execution snapshot

- [x] Add `TaskExecutionSnapshot` containing task, requirement, plan, run and projection revisions plus authoritative digests.
- [x] Require expected revisions and idempotency keys for mutating commands.
- [x] Return current snapshot, changed fields and safe-retry policy on conflict.
- [x] Display, but never infer, the snapshot in UI projections.

## AF-TGUI7 — Run Supervisor execution integration

- [x] Add `SessionRunWorker`, command dispatcher, lease heartbeat and recovery coordinator.
- [x] Execute claimed Runs through the production runtime composition.
- [x] Apply start, steer, queue, comment, fork, cancel, retry, reconcile and escalation commands durably.
- [x] Ensure waiting Decisions and Approvals release execution capacity.
- [x] Replace `g_agent_busy` and process-global active control with Session/Run ownership.
- [x] Prove three concurrent Sessions, client detach, restart takeover and stale fencing rejection offline; retain real browser detach in TGUI12 certification.

Implemented boundary: `ProductionLiveRuntime::session_run_executor()` is now the sole
typed bridge from a leased Session Run to the production long-task Harness. It rejects
missing Session/Conversation/Task/Turn bindings and stale leases. The production-runtime
integration test now claims a durable Run and traverses PlanApproval, Execution and
Assurance, while the empty-deliverable gate correctly prevents false completion. The
worker/supervisor tests additionally prove three-Session isolation, connection-independent
execution, restart takeover and stale-owner rejection. Deployed ProviderLive and real
browser-detach certification remain TGUI12 release evidence rather than offline claims.

Legacy Web execution now uses an exception-safe `(session_id, run_id)` lease registry:
one active Run is permitted per legacy Session, cancellation is fenced by Run identity,
and conversation state is retained per Session. The former process-global busy flag and
active control pointer have been removed. The legacy adapter still intentionally exposes
only `default`; product multi-Session execution remains the versioned Session/Run API path.
Runtime HTTP verification accepted the first Run as `legacy-1` (`202`), rejected a second
concurrent Run in the same Session (`429`), rejected cancellation with a stale Run ID
(`409`) and accepted cancellation with the active Run ID (`202`).

## AF-TGUI8 — Canonical runtime events and projections

- [x] Publish semantic, Decision, Task, Plan, Run, Effect, Artifact, Assurance and Closure events through one envelope.
- [x] Build rebuildable Session, Task, Plan, Observation, Activity, Approval, Evidence and Closure projections.
- [x] Drive Conversation and Observation Snapshot from the same run-scoped cursor.
- [x] Detect duplicate, reordered, expired-cursor and schema-incompatible events.
- [x] Route token, thinking, auxiliary, final and error events to the addressed Web Session;
  regression tests prove a second Session receives none of the targeted events.

## AF-TGUI9 — Complete versioned API

- [x] Complete Session mutation, membership, restore, purge, data and fork endpoints.
- [x] Add Task semantics, Decision, Plan, Run snapshot, Observation and Evidence endpoints.
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
- [x] Measure false high-effect routing, missed planning, unnecessary planning, unnecessary clarification and decision abandonment. The locked 18-case Offline campaign reports zero for all five aggregates and persists its evidence document; the real-provider campaign remains a separate TGUI12 Live gate.
- [ ] Run unit, repository, API, concurrency, restart, event replay, projection rebuild, security and browser E2E suites. Offline, ProcessLive, Chromium BrowserLive and Firefox Playwright BrowserLive pass; WebKit remains blocked by missing host libraries.
- [x] Verify no cross-Session message, memory, tool, artifact or event leakage. Repository tests cover Event/Projection and Plan/Evidence; Chromium BrowserLive now renders independent Memory/Tool/Artifact canaries in two Sessions and proves zero cross-display.

## AF-TGUI12 — Live certification and legacy closure

- [ ] Execute ProviderLive semantic/planning cases and real MCP/tool recovery cases.
- [x] Validate SQLite local and a real single-node PostgreSQL service profile without claiming the user-deferred multi-node topology.
- [ ] Execute crash, busy, disk, schema, provider, MCP, receipt and reconnect fault matrices.
- [ ] Validate Chromium, Firefox and WebKit plus keyboard, WCAG 2.2 AA and bilingual content. Chromium runtime/DOM/visual coverage and Firefox Playwright BrowserLive pass. WebKit cannot launch because this host lacks GTK4, Graphene, ICU 74, AVIF 16, WebP 7 and Manette; the full accessibility matrix remains.
- [x] Capture and inspect real Chromium desktop, tablet and narrow/mobile screenshots.
- [ ] Remove fixed default writes and legacy write endpoints after parity observation. `g_agent_busy` and process-global active control are already removed.

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

## Residual closure execution

The approved AF-TGUI-RC0 through RC8 implementation and evidence sequence is recorded in
`docs/superpowers/plans/2026-08-18-af-tgui-v3-residual-closure.md`. The dependency order is
normative: the formal Workbench production composition must be available before legacy
write authority is removed; deterministic Offline/ProcessLive gates remain independent
from ProviderLive; and BrowserLive UI claims require real screenshots from every available
browser family. ProviderLive and unavailable browser cells remain `NotCertified`, never
implicit passes.

### 2026-08-18 Chromium visual evidence

The legacy compatibility UI was rendered by the installed Google Chrome 143 using
`--headless=new` against a running `web_ui_demo --demo-state` process. This is visual
evidence for the compatibility client only; it does not certify Firefox/WebKit or the
formal React Workbench.

| View | Runtime file | SHA-256 | Inspection |
|---|---|---|---|
| Desktop 1440×1000 | `/tmp/af-tgui12-desktop.png` | `0b78d441ab1b19ebb2de99ebde839e5f06136498d3fe7b1df1aa26d85c77aed0` | pass |
| Tablet 820×1180 | `/tmp/af-tgui12-tablet.png` | `dde575a8d73637b81298ef527ae97dce26eea368d4b17e08587fff54b688b563` | pass |
| Narrow/mobile render | `/tmp/af-tgui12-mobile-emulated.png` | `3d10f49fb56d3680086d7ecfa1e1f1315f9c2a3a94f0a449b5b6637899029471` | pass after responsive min-width/action repair |

The first direct 390-pixel Chrome CLI capture exposed Chrome's headless minimum CSS
viewport as a cropped image rather than valid device emulation and was rejected as
evidence. The accepted narrow render uses device scaling, shows wrapped answer content,
the full composer and an accessible Send action without horizontal clipping.

The formal React Workbench is additionally certified by the `workbench_browser_live`
CTest. It starts the real SQLite runtime server, renders durable facts in Chrome, answers
the persisted Decision, observes worker resume and projection-head advance, and switches
between two Sessions carrying distinct Memory, Tool and Artifact canaries. The test fails
if either Session renders the other Session's canaries. The final post-fix rerun passed in
71.65 seconds with zero failures. The accepted final narrow screenshot is
`/tmp/af-tgui12-workbench-narrow-final.png`, SHA-256
`3a9ccde8fff2ff6ffa51fe77fc3be8b9cb8bfb39df3e6e358191b5030c788106`.

### 2026-08-18 browser-family execution evidence

The connected Playwright test now enters the fixture through the stable
`#session-orbital` route instead of relying on backend list order. Firefox 1538 executed
the real SQLite Session/Decision workflow, submitted the professional-assurance choice,
observed the durable answered state and passed in 9.0 seconds. This also exposed and
removed a non-deterministic test defect: `/` could legitimately select the independent
isolation Session first.

WebKit 2336 was downloaded and executed, but Playwright rejected launch before page
creation because the host is missing `libgtk-4.so.1`, `libgraphene-1.0.so.0`, ICU 74,
`libavif.so.16`, `libwebp.so.7` and `libmanette-0.2.so.0`. This is retained as an
environment blocker, not a product pass or a skipped success. Playwright Chromium on
this host still intermittently leaves the initial Session-list request unresolved; the
separate native-Chrome `workbench_browser_live` test is the accepted Chromium evidence.
The local PostgreSQL probe (`/var/run/postgresql:5432`) also returned no response, so the
system-managed instance is unavailable. A temporary PostgreSQL 12 server was therefore
initialized under `/tmp`, bound to a private Unix socket, and used to execute the real
`phase4_postgres_queue` suite. Queue quota, concurrent `SKIP LOCKED` claims, worker crash
takeover/fencing, backend termination/reconnect and transactional rolling migration all
passed in 0.92 seconds; the temporary server and data directory were then removed. This
certifies the single-node PostgreSQL service profile only, not multi-node topology.

### 2026-08-18 residual closure evidence

AF-TGUI-RC0 through RC8 is governed by
`docs/superpowers/plans/2026-08-18-af-tgui-v3-residual-closure.md`. The first residual
implementation batch added a locked semantic/planning campaign report at
`build-ui/agent_framework/certification/af-tgui-task-semantics-offline.json` (18 samples;
false-high-effect, missed/unnecessary planning, unnecessary clarification and controlled
decision abandonment all zero; report SHA-256
`384f8ce84a888c2e59d5c38543f5fa26f155bdea3d580bd5b182992db1c1cf89`). A real-provider
campaign executable and fail-closed launcher are built, but correctly return
`NotCertified` when provider credentials/model or a real MCP configuration are absent.

The recovery matrix revision is now `af-tgui-v3-r1`. It explicitly names worker crash,
stale completion, cancellation races, object loss/corruption, SQLite busy, storage and
disk-write failure, schema migration/incompatibility, receipt reconciliation, client
reconnect, provider disconnect/reattach and MCP disconnect/late-result cells. Evidence
levels are enforced per cell; Offline cannot satisfy ProcessLive or ProviderLive cells.

The formal Workbench now includes persisted English/Simplified-Chinese locale selection,
document-language synchronization, current-Session semantics, controlled mobile
navigation, visible keyboard focus, Task/Evidence tab semantics, Decision grouping and
status/error live regions. `@axe-core/playwright` reports zero serious or critical issues
in the connected Firefox runtime. Six connected Firefox tests passed, including keyboard,
bilingual overflow, Decision resume and three viewport captures. Screenshot evidence:

| View | Runtime file | SHA-256 | Inspection |
|---|---|---|---|
| Desktop 1440×1000 | `docs/evidence/screenshots/af-tgui-desktop.png` | `ff0f5370fa95d37d6484ee840198a6c12265060d05b8acba312e1e362e08807d` | pass |
| Tablet 820×1180 | `docs/evidence/screenshots/af-tgui-tablet.png` | `a87d41341507dc8782326801ec80c5013c1153f57b63afd123930fab30f29d5b` | pass |
| Mobile 390×844, Chinese drawer | `docs/evidence/screenshots/af-tgui-mobile-zh.png` | `85fe21b9e1045969c4a59bc201ec2c2a3c2603ee0ac6d0f71e0434fc7f778d41` | pass after opaque drawer/scrim repair |

The updated native-Chrome BrowserLive Decision/resume/isolation CTest passed in 69.48
seconds. The complete assertion-enabled Phase 4 Offline suite passed 109/109 in 13.36
seconds; JUnit evidence is
`build-ui/agent_framework/certification/af-sltr-phase4-offline-20260818T045349Z.xml`
(SHA-256 `68b23b258746e2e6a785d1754248af27fa0fe9af4b6e834f532326e0a51eba14`).
ProcessLive restart/reconnect certification also passed in 20.58 seconds.

WebKit remains an explicit environment blocker. The approved dependency installer could
not elevate because `sudo` requires an interactive password; its Ubuntu fallback also
requests packages unavailable from this host's configured distribution repositories.
This cell remains open. Legacy `/ui/*` writes also remain open because `web_ui_demo` is
still the shipped execution entrypoint; removing them before a production resource graph
backs the formal `/api/v1` Workbench would be a functional regression rather than closure.
During this mandatory parity-observation window all three legacy mutation responses now
publish `Deprecation`, `Sunset`, `Link: rel="successor-version"` and
`X-Agent-Legacy-Authority: compatibility-only` headers. This makes migration machine
visible without prematurely disabling the only shipped execution route.
The canonical `/api/v1/runs` ingress now resolves `conversation_id` from the authenticated
Product Session, rejects a client-forged cross-Conversation binding, normalizes legacy
`prompt` to `input`, and assigns stable Task/Turn correlation identifiers before enqueue.
This closes the previously observed `production_run_binding_incomplete` path without
granting the browser authority to choose Conversation scope. API and HTTP regression tests
prove idempotent replay and forged-scope rejection.

### 2026-08-18 Release-test integrity closure

The PostgreSQL run exposed that Release builds defined `NDEBUG` while much of the suite
uses `assert()` as its test primitive. In affected targets, both checks and side-effecting
expressions inside `assert()` could disappear. The test scope now explicitly undefines
`NDEBUG` for GNU, Clang, AppleClang and MSVC; an audit of generated flags found zero
remaining `agent_framework` test targets without the override.

With real assertions enabled, the first complete Phase 4 offline run found two latent
failures. Empty-delivery detection incorrectly treated receipts from prior turns as
current delivery, and general Decision resume opened the same Task/Run before the
coordinator, causing `task_turn_idempotency_conflict`. Both were repaired. The final
`af_phase4_offline_tests` certification rebuilt 107 targets and passed all 109 tests
(0 failed, 14.27 seconds); JUnit evidence is
`build-ui/agent_framework/certification/af-sltr-phase4-offline-20260818T033842Z.xml`.

### 2026-08-18 Shipped Workbench root integration

`tools/run_ui.sh --ui web` now builds `agent_framework/web` before compiling and
launching `web_ui_demo`. The shipped root `/` serves the formal React Workbench;
the compatibility client remains explicitly available at `/legacy/`. CMake no longer
mounts `examples/web_ui_static` as the root UI. The ProcessLive test asserts all three
surfaces: the formal root title, the legacy compatibility mount and the durable default
Session returned by `/api/v1/sessions`.

The shipped local Workbench host now owns a persistent Product Session catalog, Run
supervisor, Conversation/Event store, Task registry, Decision store, Planning store and
revision-aware Interaction projection. `/api/v1/runs` is consumed by a background
SessionRun worker which resolves the authoritative Session and invokes the
harness-supported conversation path with an explicit tenant/Conversation/Session/Task/
Run/Turn binding. No process-global environment mutation is used to route concurrent
turns. Conversation events preserve the selected Run identity, Session Data exposes the
authenticated durable user/assistant messages, and runtime events are incrementally
projected into the same Workbench cursor. `model_stop` with `end_turn` now projects as
`passed`, rather than remaining visually `running`.

The production Vite build, Conversation/Session/API/projector regression set, ProcessLive
restart/replay test and native-Chrome BrowserLive Decision/resume/Session-isolation test
all pass. BrowserLive uses an isolated Chrome profile so a user's interactive Chrome
process cannot invalidate the test. Real rendering also found and repaired three UI
defects: raw millisecond timestamps, a mobile rail remnant caused by mismatched dynamic
width/offset, and the locale control obscuring the mobile Start action. Activity cards
again expose durable fact summaries instead of only labels and states.

| View | Runtime file | SHA-256 | Inspection |
|---|---|---|---|
| Shipped root, desktop 1440×1000 | `/tmp/af-workbench-root-desktop-final.png` | `7372c696d8c3ce5eaf8970407eebffbb21b73e738d03a6d591b40c36bad671eb` | pass; messages, Run r5 and event/projection head 24 converge |
| Shipped root, responsive 500×844 | `/tmp/af-workbench-root-mobile-final.png` | `a9b85d39b08876809b0349b43d86acc7994e01f1df0a4884ad2fe0f143a67545` | pass; no rail remnant or action occlusion |

This closes the reported “`run_ui.sh --ui web` still opens the old interface” defect,
but it does not certify the remaining ProviderLive cells or an externally authenticated
multi-user deployment. `web_ui_demo` currently supplies an authenticated local deployment
subject and a harness-supported executor; it is not evidence that every resource is owned
by `ProductionLiveRuntime`. Legacy mutation endpoints therefore remain compatibility-only
during the observation window and continue to carry deprecation headers.

### 2026-08-18 AF-WMD0–WMD7 Markdown, layout and runtime-control closure

The formal Workbench now renders assistant Markdown through a single safe pipeline:
GFM tables and task lists, syntax-highlighted fenced code, KaTeX inline/display math and
strict-mode Mermaid diagrams are supported. Raw model HTML is not enabled, links use the
default URL sanitizer, external links receive `noopener noreferrer`, and a browser canary
proves that a model-supplied `<script>` is displayed as inert text rather than inserted into
the DOM. User messages intentionally remain plain text.

The application shell owns exactly one viewport. Conversation history is the only vertical
scroller in the center pane, the evidence drawer and Session rail scroll independently, and
the composer remains reachable on desktop and mobile. Live messages follow the tail only
while the operator is already near it; manual upward scrolling disables follow mode and
exposes an explicit **Jump to latest** action. Pending Decisions do not force the viewport
away from the required operator control.

The left rail now owns the complete local Product Session lifecycle: create, rename, trash,
restore and two-phase permanent purge. Every mutation carries the authoritative expected
revision; purge requires an already-trashed Session plus exact-title confirmation. The rail
also exposes the service-derived runtime principal and opens Profile or System settings as
deep-linkable panels.

`/api/v1/me` and `/api/v1/runtime/settings` are backed by the authenticated runtime subject
and a tenant-scoped SQLite store. The settings contract is typed, revision-aware and audited.
It covers Provider/Model/endpoint/credential status, MCP, Skills, tool sandbox, filesystem
root, working directory, planning depth, memory strategy, Assurance tier, Judge mode,
logging/redaction, observability, theme and language. Stale writes fail through CAS.
Credential values are neither returned nor persisted through this API; unknown secret-like
keys are rejected. Theme/language are client-dynamic, while deployment fields truthfully
report that a runtime restart is required; this milestone does not claim hot reconfiguration
of an already-created `ProductionLiveRuntime`.

Final verification used the production Vite build, the shipped C++ targets and the formal
HTTP/browser surfaces. `session_run_api`, `runtime_settings_api`,
`session_run_http_api`, `workbench_browser_live` and `af_sltr_process_live` passed 5/5 in
60.14 seconds. BrowserLive proves Markdown structure, KaTeX, Mermaid, XSS rejection,
Decision resume, Session isolation, Profile and every settings group. Runtime-settings unit
coverage additionally proves persistence, CAS conflict, read-only credentials and secret-key
rejection.

| View | Runtime file | SHA-256 | Inspection |
|---|---|---|---|
| Desktop Markdown/layout 1440×1000 | `/tmp/af-wmd-final-desktop.png` | `89b2411b4b5f570c5b29d35a1b7cbec049f322b5086fa32f17d8b7b843279091` | pass; GFM, code, math, Mermaid, composer and independent panes visible |
| Runtime settings 1440×1000 | `/tmp/af-wmd-final-settings.png` | `32c06818bd1757ad9315ea39491b3f0c2877bdc4ece6ddaebc1362b9e409ab24` | pass; typed groups, revision and authorization visible |
| Mobile conversation 390×844 | `/tmp/af-wmd-final-mobile.png` | `ba8949f9f9c6c9b247b40b498e21cb9db354699561b30542ec2405d86cdcf4b3` | pass; content and composer remain independently reachable |
| Mobile Session rail 390×844 | `/tmp/af-wmd-final-mobile-rail.png` | `203797f535538ae3c2b7e7cc58ca226020c7bde6d4bacee967ec95d6a2781fd9` | pass; lifecycle, identity and settings entry points visible |

### 2026-08-18 AF-NUI0–NUI7 native UI parity closure

The FTXUI and ImGui demos now consume a shared, revision-aware native Workbench
controller backed by the authoritative Product Session API and runtime-settings store.
Both expose Session lifecycle, runtime identity, typed settings and the seven Web
Workbench context views. Selected Product Session identity is also passed into the
harness-supported conversation path; switching Sessions resets transient renderer/thread
state rather than leaking the previous Session's in-memory turn.

FTXUI rendering now wraps by Unicode terminal cells rather than bytes or spaces. Automated
coverage includes unspaced Chinese, wide glyphs, emoji/combining glyphs, explicit newlines
and long unbroken URLs. The targeted native UI regression set passed 13/13. Release TUI
and ImGui binaries were then launched in isolated state directories and captured in real
X displays. The authoritative implementation and screenshot evidence is recorded in
`docs/gui.md` section 22 and `docs/evidence/screenshots/af-nui/`.
