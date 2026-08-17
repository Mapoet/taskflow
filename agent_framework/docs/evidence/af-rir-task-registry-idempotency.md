# AF RIR Task/Run Registry Idempotency Closure

**Date:** 2026-08-17

## Incident and root cause

The second interactive turn could fail with
`task_registry_mutation_failed:UNIQUE constraint failed: task_run_links...`.
The shared demo bootstrap reused `PersistentTask::current_run_id` for every
non-control turn, while `append_requirement()` created a new `TaskRunLink` for
each requirement revision. The two contracts necessarily collided on
`(tenant, conversation, task_id, run_id)`.

A read-only audit of the historical Web database confirmed the incompatible
shape: one active Task had requirement revisions 1–10 and ten Turn links, all
pointing to the initial Run, while `task_run_links` retained only the initial
row. Historical records were not rewritten.

## Closed contract

- Executable turns without an explicit operator Run ID use their unique Turn
  ID as the new Run ID.
- Status/cancel/suspend commands retain the active Run ID.
- `AGENT_RUN_ID`, when supplied, remains a caller-owned idempotency key; reuse
  with different content fails with a stable domain conflict.
- Exact replay of Task creation, requirement append, Turn attachment, Run bind
  and Plan bind succeeds without a second revision or event.
- Same idempotency key with different content/digests fails with a typed error:
  `task_create_idempotency_conflict`, `task_turn_idempotency_conflict`,
  `task_run_link_conflict`, `task_run_plan_digest_conflict`, or
  `task_run_plan_revision_stale`.
- Raw SQLite UNIQUE diagnostics are translated at the TaskRegistry boundary and
  are no longer exposed as the public mutation result.

## Verification

- Registry/Orchestrator targeted tests: 2/2 Passed.
- Conversation/Planning/Harness regression: 9/9 Passed.
- Concurrent same-revision mutation: exactly one CAS winner; the loser receives
  `task_revision_conflict`; no partial requirement/Turn/Run rows remain.
- Commit-response-loss simulations replay create/requirement/Run/Plan operations
  successfully with no duplicate revision.
- Five production-facing binaries rebuilt: Web, TUI, ImGui, CLI and AgentServer.
- Complete Offline suite: 119/119 Passed; Phase 4 Offline: 98/98 Passed.
- Real Web binary and Chrome rendering evidence:
  `docs/assets/ui/rir-task-run-idempotency.png`.

The UI run used isolated temporary Phase 4 state. Two default demo SQLite files
touched by the binary were restored byte-for-byte from `HEAD`; temporary audit
WAL/SHM files were removed after confirming no process held them.
