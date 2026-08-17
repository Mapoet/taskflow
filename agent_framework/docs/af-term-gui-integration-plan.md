# AF Task Semantics and GUI Integration Plan

**Goal:** Sequence AF-Term v2 and GUI v2 so task understanding, Session identity, execution and user-visible evidence form one coherent production system.

## Dependency order

```text
TERM0 → TERM1 → TERM2 → TERM3 → TERM4
                         │
                         └── GUI0 → GUI1 → GUI2
TERM5 → TERM6 → TERM7 ─────────────────┤
                                       ↓
GUI3 → GUI4 → GUI5 → GUI6 → GUI7 → GUI8 → GUI9 → GUI10 → GUI11 → GUI12
```

## Batch F0 — Reliable semantic and Session foundation

- TERM0–TERM4
- GUI0–GUI2

Exit: no keyword escalation; uncertainty is durable AwaitingInput; Product Session and RuntimeSubject exist; checkpoint storage stays separate.

## Batch F1 — Concurrent execution and transport

- TERM5–TERM7
- GUI3–GUI5

Exit: real-model classification is certified; three Sessions run concurrently; commands/events are authorized, idempotent and replayable; controls are capability-driven.

## Batch F2 — Formal workbench

- GUI6–GUI7

Exit: routed Session workbench and run-scoped Activity/Plan/Memory/Approval/Evidence pass real-browser E2E and screenshots.

## Batch F3 — Enterprise governance and collaboration

- GUI8–GUI11

Exit: OIDC/RBAC, settings snapshots, memory governance, two-user collaboration and Agent/Skill/Workflow management pass security and conflict tests.

## Batch F4 — Production closure

- GUI12

Exit: ProviderLive, PostgreSQL, restart/chaos, accessibility, browser and migration evidence pass; legacy write paths are disabled.

## Mandatory evidence per batch

1. Contract and state-machine review.
2. Unit, negative, boundary and concurrency tests.
3. SQLite reopen/restart and CAS tests.
4. API/workflow/projection integration tests.
5. Authorization, replay and stale-revision tests.
6. Metrics, traces and audit evidence.
7. Real browser screenshots for UI changes.
8. Real LLM/MCP/tool tests where applicable.
9. Build target proof and full relevant CTest regression.
10. Documentation status updated without claiming a higher evidence level.
