# GUI Identity and Lifecycle RFC v1

## Object boundaries

`ProductSession` is the user-visible work container. `Conversation` is its
ordered dialogue stream. `Task` is a durable requirement lineage. `Run` is one
execution attempt, `Turn` is one conversation state-machine pass, and runtime
checkpoints are executor recovery data. None of these identifiers is an alias
for another.

Every production request and event carries a `RuntimeSubject`: tenant,
organization, principal, project, workspace, Product Session, Conversation,
Task, Run, Turn, Agent and authorization revision. Optional identifiers may be
absent before the corresponding object exists; present identifiers must form a
consistent, tenant-scoped chain. Legacy local rows can be admitted only through
an explicitly marked legacy adapter.

## Session lifecycle

`active → archived → active`, `active|archived → trashed → active`, and
`trashed → purge_pending → purged` are the only lifecycle paths. Rename, tags,
pin and folder moves increment the Session revision without changing lifecycle.
Purge is asynchronous and cannot be reversed after the worker has committed
`purged`.

## Mutation and authorization rules

- Mutations require an authenticated principal, a current authorization
  revision and an expected resource revision.
- Commands are idempotent by `(tenant, session, command_id)`.
- UI controls are derived from server capability records; disabled controls
  include a machine-readable reason.
- Events and artifacts use the same `RuntimeSubject` as their command/run.
- UI state, model text and legacy `/ui/*` routes are never authority or
  completion sources.

## Stable error contract

`identity_required`, `identity_inconsistent`, `legacy_identity_forbidden`,
`authentication_required`, `authorization_revision_required`,
`cross_tenant_forbidden`, `revision_conflict`, `idempotency_conflict`,
`invalid_lifecycle_transition`, `resource_not_found`, `resource_gone`.

## Evidence levels

`source` means implemented source exists; `offline` means deterministic tests;
`runtime` means real process/API/browser evidence; `provider_live` means real
external provider evidence; `production` additionally requires deployment,
security, recovery and SLO certification. Documentation must state the actual
level rather than infer it from code coverage.
