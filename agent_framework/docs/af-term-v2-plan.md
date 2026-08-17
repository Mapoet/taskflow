# AF Task Semantics v2 Implementation Plan

> **For agentic workers:** Execute tasks in order, keep every change independently testable, and do not let classification grant authority.

**Goal:** Replace keyword-based task escalation with a strict, LLM-driven, durable task-intent and profile-confirmation workflow.

**Architecture:** Separate user intent, execution profile, effect class, and authorization. The LLM proposes a versioned semantic decision; strict schema validation either accepts it or creates a durable clarification request. Conversation and task orchestration resume from that request without starting a high-side-effect Run prematurely.

**Tech Stack:** C++20, nlohmann/json, SQLite, existing LLM Runtime, Conversation/Task Registry, telemetry and CTest.

## Global constraints

- A task classifier never grants Tool, filesystem, network, credential, approval, or deployment authority.
- Invalid or uncertain model output fails closed into durable clarification.
- No substring heuristic may write a high-side-effect execution profile.
- Active-task input is classified as an incremental intent, not automatically as a new task.
- Every decision is versioned, observable, restart-safe and revision-checked.

## TERM0 — Semantic contract and authority separation

**Files:**

- Modify `include/agent/conversation/task_classifier.hpp`
- Create `include/agent/conversation/task_intent.hpp`
- Modify `src/conversation/task_classifier.cpp`
- Test `tests/test_task_classifier.cpp`

**Produces:** `TaskIntentKind`, `TaskIntentDecision`, `TaskClassificationEnvelope`, optional deployment override, and explicit statement that authorization is downstream.

- [x] Add intent kinds for new, continue, add requirement, narrow scope, replan, pause, cancel, status and profile confirmation.
- [x] Separate `ExecutionProfileSuggestion` from `EffectClass` and authorization.
- [x] Replace the ambiguous `Conversation == no override` API with `std::optional<TaskExecutionProfile>`.
- [x] Add JSON round-trip and non-escalation tests.
- [x] Run `cmake --build build-ui --target test_task_classifier -j2` and the matching CTest.

## TERM1 — Strict structured output

**Files:**

- Modify `include/agent/conversation/task_classifier.hpp`
- Modify `src/conversation/task_classifier.cpp`
- Test `tests/test_task_classifier.cpp`

**Consumes:** TERM0 semantic types.

**Produces:** `parse_task_classification_v2(std::string_view)` with exact required fields, bounded strings, runtime-owned classifier metadata and stable error codes.

- [x] Write failing tests for fenced JSON, multiple objects, trailing prose, missing/unknown fields, invalid enum/confidence, spoofed classifier ID and oversized output.
- [x] Implement exact JSON parsing; do not extract the first/last brace range.
- [x] Validate schema version, intent, profile, long-running recommendation, confidence, rationale and linguistic evidence.
- [x] Stamp classifier and prompt revisions in trusted runtime code.
- [x] Run focused tests and `git diff --check`.

## TERM2 — LLM linguistic cognition v2

**Files:**

- Modify `src/conversation/task_classifier.cpp`
- Create `tests/data/task-classification-v2.jsonl`
- Create `tests/test_task_classifier_eval.cpp`

**Produces:** prompt `llm-task-classifier-v2` and a reproducible multilingual evaluation corpus.

- [ ] Encode negation, scope, imperative-versus-mention, mixed intent, word boundaries, bilingual input and uncertainty rules in the system prompt.
- [ ] Require requested, negated, mention-only, scope and ambiguity evidence.
- [ ] Treat `has_active_task=true` as incremental intent classification.
- [ ] Add at least the complete positive, negative and confirmation cases from `af-term.md`.
- [ ] Enforce zero high-side-effect false positives and a versioned evaluation report.

## TERM3 — Durable profile clarification

**Files:**

- Create `include/agent/conversation/task_profile_clarification.hpp`
- Create `src/conversation/sqlite_task_profile_clarification.cpp`
- Create `tests/test_task_profile_clarification.cpp`
- Modify `CMakeLists.txt`

**Produces:** `TaskProfileClarificationStore`, `SQLiteTaskProfileClarificationStore`, CAS transitions and `parse_profile_confirmation`.

- [x] Define Pending, Confirmed, Exhausted, Expired and Cancelled states.
- [x] Persist identity, task, recommendation, allowed tokens, attempt count, revision, expiry and decision reference.
- [x] Accept exactly one canonical token (`conversation`, `read_only_analysis`,
  `artifact_delivery`, `code_change`, `external_action`, `professional`); reject
  negated, multiple and free-form answers.
- [x] Stop after three failed attempts and remain Conversation.
- [~] Test expiry and process reopen/recovery; cross-process CAS race stress remains in TERM4.

## TERM4 — Conversation, task and run integration

**Files:**

- Modify `include/agent/conversation/task_orchestrator.hpp`
- Modify `src/conversation/task_orchestrator.cpp`
- Modify `src/conversation/task_control_service.cpp`
- Modify `examples/common/agent_example_bootstrap.hpp`
- Test `tests/test_task_orchestrator.cpp`
- Test `tests/test_conversation_wait_states.cpp`

**Produces:** intent-aware task routing and `AwaitingInput` clarification/resume.

- [ ] Route continue/add/narrow/replan into the active Task; route pause/cancel/status to control services.
- [ ] Create a new Task only for `NewTask`.
- [ ] Persist clarification before returning AwaitingInput.
- [ ] Resume through an idempotent command after confirmation.
- [ ] Link classification and clarification decisions to TaskRunLink.
- [ ] Verify no duplicate Task/Run after retries or restart.

## TERM5–TERM7 — Observability, UI and Live certification

**Files:** telemetry runtime, UI presentation model, Web/TUI/ImGui/CLI handlers, live evidence documentation.

- [ ] Emit classification and clarification lifecycle events without raw sensitive prompts.
- [ ] Publish latency, token, cost, confusion, confidence calibration and clarification-rate metrics.
- [ ] Render the same canonical clarification in all four clients.
- [ ] Run multilingual real-provider classification, malformed output, timeout and restart scenarios.
- [ ] Retire deterministic profile escalation only after Live certification passes.

## Acceptance

- High-side-effect false-positive count is zero in the locked corpus.
- Invalid/uncertain output always waits for confirmation and survives restart.
- Repeated confirmation does not duplicate a Task or Run.
- Classification never widens authorization.
- Source, build, offline, runtime and ProviderLive evidence are reported separately.
