# AF Task Semantics and GUI Traceability

Status date: 2026-08-18

Evidence levels: `S` source, `B` built, `O` offline test, `R` real runtime, `L` Provider/Tool Live, `P` production certified.

| Requirement | Current evidence | Level | Remaining proof |
|---|---|---:|---|
| No keyword-based authority escalation | Task semantics v4, strict parser, conservative fallback and multilingual routing tests | O | ProviderLive false-positive evaluation |
| Relevant dynamic clarification | general durable Decision store/coordinator, typed semantic patches, CAS answer/cancel/expiry/reopen migration | O/R | all non-Web clients and ProviderLive ambiguity calibration |
| Task intent and durable requirement history | TaskIntentKind, promotion policy, TaskOrchestrator and SQLite TaskRegistry | O | complete multiple-Task Session workbench UX |
| LLM task cognition and Plan binding | independent work/effect/assurance axes, typed planning decision and requirement revision binding | O | ProviderLive calibration and production planner invocation matrix |
| LLM invocation observability | classifier per-call observer publishes provider/model/prompt/deployment, digests, latency, usage, confidence and fallback without raw prompt; text-free calibration aggregate exposes false high-effect, planning, clarification and abandonment rates | B/O-partial | persist all workflow roles through one LLMRuntimeStore and run ProviderLive calibration campaigns |
| Product Session catalog | create/list/get/rename/organize/member/lifecycle/fork/restore/two-phase purge/data APIs and CAS tests | O | background physical-erasure receipt |
| Versioned Task API | durable Task aggregate plus semantics, Decision, Plan, Run/execution snapshot, Observation and Evidence endpoints; capability and OpenAPI contracts | O/R | browser/provider-live certification |
| Per-Session Run supervision | Run Worker, lease/heartbeat/fencing, parked waits, all typed commands and atomic child-Run fork | O/R | removal of legacy demo globals |
| Production Session Run bridge | `ProductionLiveRuntime::session_run_executor`; a leased durable Run traverses production PlanApproval, Execution and Assurance with empty-delivery fail-closed | S/O | deployed ProviderLive Harness execution |
| Authorized replay transport | HTTP/SSE/WebSocket replay plus typed interaction and execution-snapshot APIs | O/R | sustained slow-consumer and reconnect browser certification |
| Unified RuntimeSubject | typed subject and API validation; Planning/Evidence v2 keys include conversation scope with tested v1 migration and same-task cross-Session isolation | O | full Tool/Memory/Artifact end-to-end isolation |
| Authoritative completion | TaskClosureController and production runtime integration | O | all entrypoints and Live recovery evidence |
| Observation consistency | canonical Conversation events project one-for-one to typed interactions; runtime/projection heads converge and isolation is tested | O/R | continuous projector wiring in every production entrypoint and rebuild-from-retention recovery |
| Runtime event integrity | write-time sequence enforcement plus read-time digest/schema/scope/order diagnostics and projection cursor checks | O | remote/multi-node fault injection |
| Formal multi-Session workbench | React/Vite/TanStack app consumes capability, event, interaction, Decision, Run and execution snapshot APIs | B/R-partial | replace legacy demo write path; browser E2E and screenshots |
| OIDC/RBAC/collaboration | partial policy and API authorization primitives | S/O-partial | real IdP, presence, conflict and revocation certification |
| Settings, credentials and memory governance UI | backend components are distributed across Phase 4 modules | S/O-partial | effective config API and formal UI |
| ProviderLive classification and planning | no current evidence accepted | — | multilingual real-provider matrix |
| Production multi-node service profile | explicitly deferred by user | — | remains outside current certification claim until resumed |

## Known contradictions to remove

1. `af-term.md` and the TERM3 wording still describe a fixed six-profile confirmation, while current product semantics require dynamic LLM-generated choices only for material ambiguity.
2. `TaskExecutionProfile` still determines long-task routing even though work duration, effect risk and assurance depth are independent axes.
3. The versioned Workbench runtime is revision-aware, but `web_ui_demo` still uses `default`, `g_agent_busy` and one process-global control.
4. The formal React client consumes `/api/v1`; the shipped legacy demo static client still consumes `/ui/*` and therefore is not parity-certified.
5. Canonical event/projection convergence is proved for the new runtime fixture, but not yet wired continuously through every legacy/production entrypoint.
6. The production runtime can now export a typed Session worker executor, but no shipped UI service has yet supplied its complete production resource graph for a positive Live Run.

## Executed evidence in this batch

- Phase 4 labeled CTest: 121 selected, 120 passed, 0 failed, 1 environment-gated PostgreSQL queue test skipped (37.78 s).
- Focused CTest: production Session Run bridge, task semantics, routing, Decision, Task orchestration, execution snapshot, Run supervisor/worker/API, event integrity, projection and harness-supported runtime.
- Runtime HTTP: capability manifest, typed interactions and execution snapshot returned consistent heads; answering a durable Decision woke the parked Run and completed it under a new lease epoch.
- Runtime HTTP: Session fork created independent Session/Conversation identities against the expected source revision; Session data reported matching runtime and interaction heads.
- Runtime HTTP: Task semantics, current Plan, task observations and Evidence endpoints each returned 200 from the formal Workbench service; the Capability Manifest advertised the same resources.
- SQLite Planning/Evidence schema v2: conversation-scoped physical keys, v1 data migration, restart, and same-tenant/same-task/same-object-id Session isolation passed.
- Run command matrix: start, steer, queue, comment, fork, cancel, retry, reconcile and escalation persistence/dispatch passed; Run fork creates its child Run and Start command atomically.
- React production build: TypeScript and Vite completed successfully; active input now steers the current revision instead of incorrectly opening a new Run.
- SQLite boundary: operational calls and schema introspection are centralized in `agent/internal/sqlite_utils.hpp` and enforced by a source-boundary test.

## Explicit blockers / non-claims

- The environment does not expose the browser-control interface required by the mandated browser skill. Chromium/Firefox/WebKit E2E and real desktop/tablet/mobile screenshots are therefore not executed and must not be represented as passed.
- ProviderLive/MCP fault certification and PostgreSQL multi-node topology are not established by offline tests.
- `web_ui_demo` legacy writes and process-global busy/control remain until production Harness/Run Supervisor parity is demonstrated.

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
