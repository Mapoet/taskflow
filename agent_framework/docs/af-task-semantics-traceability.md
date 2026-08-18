# AF Task Semantics and GUI Traceability

Status date: 2026-08-18

Evidence levels: `S` source, `B` built, `O` offline test, `R` real runtime, `L` Provider/Tool Live, `P` production certified.

| Requirement | Current evidence | Level | Remaining proof |
|---|---|---:|---|
| No keyword-based authority escalation | Task semantics v4, strict parser, conservative fallback and multilingual routing tests | O | ProviderLive false-positive evaluation |
| Relevant dynamic clarification | general durable Decision store/coordinator, typed semantic patches, CAS answer/cancel/expiry/reopen migration | O/R | all non-Web clients and ProviderLive ambiguity calibration |
| Task intent and durable requirement history | TaskIntentKind, promotion policy, TaskOrchestrator and SQLite TaskRegistry | O | complete multiple-Task Session workbench UX |
| LLM task cognition and Plan binding | independent work/effect/assurance axes, typed planning decision and requirement revision binding | O | ProviderLive calibration and production planner invocation matrix |
| LLM invocation observability | classifier per-call observer publishes provider/model/prompt/deployment, digests, latency, usage, confidence and fallback without raw prompt; locked 18-case campaign persists all five text-free aggregates with zero Offline regressions | B/O | persist all workflow roles through one LLMRuntimeStore and run the built ProviderLive campaign |
| Product Session catalog | create/list/get/rename/organize/member/lifecycle/fork/restore/two-phase purge/data APIs and CAS tests | O | background physical-erasure receipt |
| Versioned Task API | durable Task aggregate plus semantics, Decision, Plan, Run/execution snapshot, Observation and Evidence endpoints; capability and OpenAPI contracts | O/R | browser/provider-live certification |
| Per-Session Run supervision | Run Worker, lease/heartbeat/fencing, parked waits, all typed commands and atomic child-Run fork; legacy Web compatibility path now uses exception-safe `(session_id, run_id)` leases and per-Session state | O/R | ProviderLive detach/reconnect and deployed takeover proof |
| Production Session Run bridge | `ProductionLiveRuntime::session_run_executor`; canonical Run ingress derives Conversation scope and stable Task/Turn bindings server-side, rejects forged scope, and a leased durable Run traverses production PlanApproval, Execution and Assurance with empty-delivery fail-closed | S/O | deployed ProviderLive Harness execution |
| Authorized replay transport | HTTP/SSE/WebSocket replay plus typed interaction and execution-snapshot APIs | O/R | sustained slow-consumer and reconnect browser certification |
| Unified RuntimeSubject | typed subject and API validation; Planning/Evidence v2 keys include conversation scope with tested v1 migration and same-task cross-Session isolation; BrowserLive renders distinct Memory/Tool/Artifact canaries in two Sessions without leakage | O/R | remote/multi-node isolation certification |
| Authoritative completion | TaskClosureController and production runtime integration | O | all entrypoints and Live recovery evidence |
| Observation consistency | canonical Conversation events project one-for-one to typed interactions; runtime/projection heads converge and isolation is tested | O/R | continuous projector wiring in every production entrypoint and rebuild-from-retention recovery |
| Runtime event integrity | write-time sequence enforcement plus read-time digest/schema/scope/order diagnostics and projection cursor checks | O | remote/multi-node fault injection |
| Formal multi-Session workbench | React/Vite/TanStack app consumes typed APIs; native Chromium and Firefox BrowserLive pass; Firefox axe serious/critical=0, keyboard Decision focus, persisted English/Chinese locale and inspected desktop/tablet/mobile screenshots pass | B/R | WebKit host dependencies and replacement of legacy demo writes |
| OIDC/RBAC/collaboration | partial policy and API authorization primitives | S/O-partial | real IdP, presence, conflict and revocation certification |
| Settings, credentials and memory governance UI | backend components are distributed across Phase 4 modules | S/O-partial | effective config API and formal UI |
| ProviderLive classification and planning | no current evidence accepted | — | multilingual real-provider matrix |
| PostgreSQL service profile | real PostgreSQL 12 single-node queue/quota/claim/crash fencing/reconnect/rolling-migration suite passed | R | multi-node topology remains explicitly deferred by user |

## Remaining contradictions to remove

1. `af-term.md` and the TERM3 wording still describe a fixed six-profile confirmation, while current product semantics require dynamic LLM-generated choices only for material ambiguity.
2. `TaskExecutionProfile` still determines long-task routing even though work duration, effect risk and assurance depth are independent axes.
3. The formal React client consumes `/api/v1`; the shipped compatibility client still uses the fixed `default` Session and `/ui/*`, although its run ownership and event dispatch are now Session/Run scoped.
4. Canonical event/projection convergence is proved for the production API fixture and legacy dispatch isolation, but deployed ProviderLive entrypoint recovery is not certified.
5. The production runtime exports a typed Session worker executor, but no deployed UI service has supplied a complete real-provider resource graph for a positive ProviderLive Run.

## Executed evidence in this batch

- Earlier Phase 4 baseline: 121 selected, 120 passed, 0 failed, 1 PostgreSQL test skipped. This is superseded by the assertion-enabled offline and PostgreSQL Live evidence below.
- Focused CTest: production Session Run bridge, task semantics, routing, Decision, Task orchestration, execution snapshot, Run supervisor/worker/API, event integrity, projection and harness-supported runtime.
- Runtime HTTP: capability manifest, typed interactions and execution snapshot returned consistent heads; answering a durable Decision woke the parked Run and completed it under a new lease epoch.
- Runtime HTTP: Session fork created independent Session/Conversation identities against the expected source revision; Session data reported matching runtime and interaction heads.
- Runtime HTTP: Task semantics, current Plan, task observations and Evidence endpoints each returned 200 from the formal Workbench service; the Capability Manifest advertised the same resources.
- SQLite Planning/Evidence schema v2: conversation-scoped physical keys, v1 data migration, restart, and same-tenant/same-task/same-object-id Session isolation passed.
- Run command matrix: start, steer, queue, comment, fork, cancel, retry, reconcile and escalation persistence/dispatch passed; Run fork creates its child Run and Start command atomically.
- React production build: TypeScript and Vite completed successfully; active input now steers the current revision instead of incorrectly opening a new Run.
- SQLite boundary: operational calls and schema introspection are centralized in `agent/internal/sqlite_utils.hpp` and enforced by a source-boundary test.
- Legacy Web ownership: the process-global busy flag/control pointer were removed; runtime HTTP accepted one Session/Run lease, rejected concurrent and stale-run mutations, and accepted the correctly fenced cancellation.
- Formal Workbench BrowserLive: native Chrome rendered two isolated Sessions, answered a durable Decision, observed worker resume and completion, and passed the final post-fix CTest in 71.65 seconds. Real desktop/tablet/narrow screenshots were captured and inspected.
- Browser family: Playwright Firefox 1538 executed the connected durable workflow and passed in 9.0 seconds. The test now selects `#session-orbital` explicitly instead of depending on Session list order.
- Release test integrity: the test directory now undefines `NDEBUG`, preventing `assert()` checks and side-effecting test operations from disappearing. Generated-flag audit found zero remaining affected Agent Framework test targets.
- Full Phase 4 offline certification after restoring assertions: 109/109 passed, 0 failed; 107 build targets; JUnit `build-ui/agent_framework/certification/af-sltr-phase4-offline-20260818T033842Z.xml`.
- PostgreSQL service Live: a private temporary PostgreSQL 12 instance executed quota, concurrent claim, crash takeover/fencing, reconnect and rolling-migration coverage; `phase4_postgres_queue` passed in 0.92 seconds and the temporary instance was removed.
- AF-TGUI semantic campaign: 18/18 locked cases passed the planning-policy oracle; all five calibration aggregates are zero; evidence SHA-256 `384f8ce84a888c2e59d5c38543f5fa26f155bdea3d580bd5b182992db1c1cf89`.
- Recovery matrix `af-tgui-v3-r1`: explicit disk, incompatible-schema, receipt, client reconnect and MCP cells were added; required evidence levels reject skip-as-pass.
- Accessible bilingual Workbench: connected Firefox ran six tests successfully, including axe, keyboard focus, locale persistence, overflow, Decision resume and visual viewports. The first mobile capture exposed a translucent/mid-transition drawer and was rejected; the repaired opaque drawer and scrim were recaptured and inspected.
- Current regressions: native Chrome BrowserLive passed in 69.48 seconds; ProcessLive passed in 20.58 seconds; final Phase 4 Offline passed 109/109 in 13.36 seconds with JUnit SHA-256 `68b23b258746e2e6a785d1754248af27fa0fe9af4b6e834f532326e0a51eba14`.

## Explicit blockers / non-claims

- WebKit 2336 cannot launch because this host lacks GTK4, Graphene, ICU 74, AVIF 16, WebP 7 and Manette runtime libraries. The approved Playwright installer cannot elevate through interactive `sudo`, and its Ubuntu fallback package set is unavailable in the configured repositories. This is an environment blocker, not a skipped pass.
- Playwright Chromium intermittently leaves the initial Session request unresolved on this host; the independent native-Chrome CTest is the accepted Chromium BrowserLive evidence.
- ProviderLive/MCP fault certification is not established by offline tests. No provider credential is assumed.
- The system-managed PostgreSQL socket returned no response, but a private temporary PostgreSQL 12 instance passed the single-node service-profile suite. The user-deferred multi-node topology is not claimed.
- `web_ui_demo` still exposes compatibility writes for the fixed `default` Session. Its process-global busy/control state is removed and every mutation now emits deprecation/sunset/successor/compatibility-only headers, but endpoint removal waits for production Workbench parity observation.

## Required test layers

1. Contract/schema and negative parsing tests.
2. SQLite CAS, reopen, migration and cross-connection races.
3. Task/Decision/Plan/Run integration and idempotent replay.
4. Three-Session concurrency, cancellation and lease takeover.
5. API authorization, stale revision, replay and cursor expiry.
6. Projection duplicate/reorder/rebuild and crash injection.
7. Real browser E2E and screenshots at required widths.
8. ProviderLive and real Tool/MCP recovery evidence.
9. Full relevant CTest regression and build-target proof.
