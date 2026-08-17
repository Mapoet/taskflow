# AF-SLTR Traceability and Certification Evidence

**Audit time:** 2026-08-16T16:03:59Z (2026-08-17 Asia/Shanghai)  
**Requirement baseline:** `docs/af-session-long-task-integration-closure-v2.md`  
**Offline JUnit:** `build-ui/agent_framework/certification/af-sltr-phase4-offline-20260816T160800Z.xml`  
**Result:** 98/98 Phase 4 offline tests Passed in a localhost-capable environment.

This file distinguishes deterministic correctness, ProcessLive evidence and
ProviderLive evidence. A missing live cell remains `NotCertified`; it is never
converted to Passed by an offline test.

## Golden-task matrix

| Golden task from `af-test.md` | Executable evidence | Certification |
|---|---|---|
| Five-minute/multi-tool/file/chart workflow | `phase4_long_task_workflow`, `phase4_execution_adapters`, `phase4_incremental_result_store` | Offline Passed; duration/provider realism NotCertified |
| Concurrent “continue” and amended requirements | `task_orchestrator`, `task_command_service`, `conversation_wait_states` | Offline Passed |
| Kill during model/tool work and restart | `phase4_recovery_certification`, `phase4_harness_restart`, `phase4_live_restart` | Offline Passed |
| Remote/MCP result lost locally; reconcile without duplicate effect | `phase4_execution_control`, `phase4_remote_queue`, `phase4_remote_queue_mtls` | Offline/localhost ProcessLive Passed; real MCP late-result NotCertified |
| Approval wait and resume | `phase4_approval_executor`, `phase4_harness_restart`, `harness_turn_adapter` | Offline Passed; ten-minute wall-clock campaign NotCertified |
| Two context compactions retain task contract | `phase4_conversation_runtime` mandatory-state digest invariant | Offline Passed |
| No-information-gain node triggers bounded replan | `phase4_long_task_workflow` | Offline Passed |
| Missing final artifact cannot complete | `phase4_harness_artifact_loop`, `phase4_task_closure` | Offline Passed |
| Empty assistant delivery cannot complete | `empty_delivery`, `harness_turn_adapter` | Offline Passed |
| Conversation failure cannot leave Harness silently running | `task_state_coordinator`, `system_state_reconciler`, `production_live_runtime` | Offline Passed; historical stores still contain pre-fix orphan records and are not rewritten |

## SLTR6 unified operations evidence

- Source authority: `ProductionLiveRuntime` publishes the durable
  `TaskStateCoordinator` decision to `LiveOperationsProjection`.
- Ordering invariant: a late Conversation `model_stop` changes only
  `response_delivery_state`; it cannot revoke semantic closure.
- Snapshot axes: response delivery, Harness pipeline and task verification are
  separately serialized, replayed and rendered.
- Release-mode tests: `live_operations_projection`,
  `phase4_conversation_runtime`, and `production_live_runtime` Passed.
- Actual UI evidence:
  - `docs/assets/ui/sltr6/web-operations.png`
  - `docs/assets/ui/sltr6/tui-operations.png`
  - `docs/assets/ui/sltr6/imgui-operations.png`
- CLI consumed the same durable Web Operations database and rendered the same
  three axes.

### Authoritative task-command actions

- `TaskCommandPolicy` enforces authenticated actor identity, exact tenant and
  conversation match, command scopes, lifecycle legality and revision CAS.
- Shared LiveRuntime publishes the resulting action set into
  `phase4.operations.v1`; production has no default principal resolver.
- Web/TUI/ImGui/CLI consume this persisted contract. Web prepares an action for
  canonical Conversation submission and does not invoke a parallel mutation path.
- Targeted command/projection/runtime suite: 6/6 PASS.
- Real Web screenshot: `docs/assets/ui/sltr6/web-task-actions.png`.

## Immutable current-store observation

The following is a read-only observation of the existing demo databases, not a
migration or cleanup. Counts include historical pre-fix executions and the two
workspace locations that already existed. Database SHA-256 values bind the
scope at the audit time.

| Database | Turn/task or Harness scope | SHA-256 |
|---|---|---|
| `.agent-framework/imgui_agent_demo-conversation.sqlite3` | 3 turns: 1 completed, 2 failed; 1 active/running task; revision 2 | `593e22d54360cb35dff61809f738bc82ccc02ef0e816fc4b6e20683dc385343a` |
| `.agent-framework/web_ui_demo-conversation.sqlite3` | 1 completed turn; 1 active/running task; revision 2 | `82f8d64fc6c2c8a074e722c5ca7a5ceb4ee444cbbac3b05a9f76fed5b81f89ac` |
| `agent_framework/.agent-framework/imgui_agent_demo-conversation.sqlite3` | 29 completed turns; 1 active/running task; task revision 17 | `51cca8011dcf277e866ebe75068d9c33ef496160d910b86f58d8783822e8a31b` |
| `agent_framework/.agent-framework/tui_agent_demo-conversation.sqlite3` | 20 turns: 15 completed, 5 failed; 1 active/running task; task revision 15 | `1c3b85a8a0f897e0dde6d4829bb63036812e42db9dbb6faf38ffeb80dcf3766c` |
| `agent_framework/.agent-framework/web_ui_demo-conversation.sqlite3` | 92 turns: 32 completed, 55 failed, 5 running; 1 active/running task; task revision 10 | `97953c4a60e064824b89e2a5dfa8090f9761c843849c5d302912f0ee782f88b1` |
| `.agent-framework/imgui_agent_demo-harness.sqlite3` | 3 completed Harness checkpoints; revision 18 | `9db79009c7b521c983930939f5e8dd0cbd36b43bde598ab0a005c916abde3f70` |
| `.agent-framework/web_ui_demo-harness.sqlite3` | 1 completed Harness checkpoint; revision 18 | `8d5cec446caddb853d7d97f9aa5e9f20fca6e1cca55423c3d3dc2bb588eb14a5` |
| `agent_framework/.agent-framework/imgui_agent_demo-harness.sqlite3` | 17 completed Harness checkpoints; revision 18 | `ff9a9277fdf5f88eaad8ebe4a002d1f1d2745c99ff57956bd074be2003182dd3` |
| `agent_framework/.agent-framework/tui_agent_demo-harness.sqlite3` | 20 completed Harness checkpoints; revision 18 | `273de681259d5d613f392fbf77daa282fe33802f06e1d8e30a610d16e283d92d` |
| `agent_framework/.agent-framework/web_ui_demo-harness.sqlite3` | 68 checkpoints: 58 completed, 1 failed, 9 historical running; revisions 8–18 | `74f53e2142b66cee24d2f7462610db46db548f1d0b825a780681bf68f84ba3e6` |

The nine historical running Harness rows remain audit evidence. New recovery
logic is tested, but this audit does not mutate old records or retroactively
claim that they were recovered.

## Certification cells

| Cell | State | Evidence / blocker |
|---|---|---|
| Offline correctness | Passed | 98/98 JUnit above |
| Localhost remote queue and native mTLS | Passed | `phase4_remote_queue` and `phase4_remote_queue_mtls`; both reject execution in the restricted network sandbox and pass in the localhost-capable environment |
| ProcessLive Web/TUI/ImGui/CLI projection | Passed | actual screenshots and CLI durable replay above |
| ProcessLive disconnect/reconnect/crash matrix | Passed | `af-sltr-process-live-20260817T065314Z.json`: two-client replay, Last-Event-ID resume, SIGKILL/restart, stable task identity and Operations revision 6→12; report SHA-256 `2193e949ef17b32559263f7d6788aa0a426cfa84ae5f669be9b57ee185e206c1` |
| ProviderLive LLM/MCP late-result/reattach | NotCertified | no complete real-provider credential/end-point campaign evidence in this audit |
| External OIDC/JWT/KMS task actions | NotCertified | local identity/policy/revision action contract Passed; real external IdP/KMS integration remains unavailable |

## Open mandatory work

1. Run the remaining fixed-seed crash/steering campaign as one versioned report
   and record orphan, divergence, duplicate-effect, empty-delivery, first
   progress and heartbeat metrics.
2. Run ProviderLive only with real provider/MCP/A2A endpoints and credentials;
   otherwise retain `NotCertified`.

## Operational metric gate

`certify_long_task_metrics()` now canonicalizes and gates the required campaign
metrics. It rejects any nonzero orphan-running, state-divergence,
duplicate-effect or empty-completed count; resume success below 99%; P95 first
progress above 10 seconds; and P95 heartbeat outside 5–15 seconds. The positive
and seven-failure negative contracts pass in `phase4_recovery_certification`.
This is the metric acceptance mechanism, not fabricated production
measurements. The localhost ProcessLive campaign now records a bounded sample:
zero observed orphan/divergence/duplicate-effect/empty-completion events, resume
1/1, first-progress P95 upper bound 2021 ms, and two observed heartbeats with
5000 ms P95 interval. These values pass the gate for this campaign only; they are
not a substitute for a statistically meaningful ProviderLive/production soak.
