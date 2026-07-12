# Workflow and Agent Control Flow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete every implementation, test, and documentation requirement in `docs/workflow-agent-control-flow-upgrade.md` so Workflow loops and reusable modules are re-entrant and Agent Framework uses explicit loop state.

**Architecture:** Replace one-shot internal `std::shared_future<std::any>` wiring for Any nodes with reusable value slots while retaining first-run futures for source compatibility. Add execution contexts, explicit module output bindings, and a new loop API whose per-run nested Taskflow uses a Taskflow 4.x conditional-task back-edge. Migrate AgentLoopNode to pass immutable iteration snapshots through loop outputs and define commit/retry/resume and bounded subflow contracts.

**Tech Stack:** C++20, Taskflow 4.1 conditional tasks, CMake, CTest, standard library `std::any`, `std::stop_token`, and existing Agent Framework mock clients.

## Global Constraints

- Preserve existing public overloads and examples; legacy overloads may delegate to the new runtime.
- The recommended APIs must never return empty `std::any` placeholders.
- A graph may be run sequentially more than once; concurrent calls on the same `GraphBuilder` must fail explicitly.
- Different graphs must remain safe when run concurrently on one executor.
- Loop exit publishes final outputs once per run; intermediate iterations use isolated state snapshots.
- Side-effecting agent tools are not automatically retried without an idempotency identity.
- Keep C++20 and do not add third-party dependencies.

---

### Task 1: Reusable Any Value Slots and Run Guard

**Files:**
- Modify: `workflow/include/workflow/nodeflow.hpp`
- Modify: `workflow/src/nodeflow.cpp`
- Create: `workflow/tests/test_runtime_slots.cpp`
- Modify: `workflow/CMakeLists.txt`

**Interfaces:**
- Produces: `AnyValueSlot::publish(std::any)`, `AnyValueSlot::read()`, `AnyOutputs::publish(key, value)`, `INode::get_output_slot(key)`, and `GraphBuilder::get_latest_output(node, key)`.
- Produces: sequentially reusable `GraphBuilder::run`; concurrent execution of one builder throws `std::logic_error`.

- [x] **Step 1: Add a failing slot/re-run test**

Create a source -> increment -> sink graph, run it three times, and assert that the sink observes `2` three times. Start two runs on the same builder and assert the second call rejects while the first is active.

- [x] **Step 2: Run the focused test and confirm the one-shot promise failure**

Run: `cmake --build build-upgrade-full -j2 --target test_workflow_runtime_slots && ctest --test-dir build-upgrade-full -R workflow_runtime_slots --output-on-failure`
Expected before implementation: FAIL with `std::future_error: Promise already satisfied` or a missing target.

- [x] **Step 3: Implement slots and migrate Any nodes**

Add thread-safe slots with generation counters. `AnySource`, `AnyNode`, `AnySink`, condition nodes, and loop nodes read/write slots internally. Keep `get_output_future` as a first-publication compatibility view.

- [x] **Step 4: Implement the graph run guard**

Use shared run state captured by the Taskflow completion callback so sequential runs reset the guard and overlapping runs throw before scheduling.

- [x] **Step 5: Run the focused test**

Expected: `workflow_runtime_slots` passes three sequential runs and the concurrent-run rejection case.

### Task 2: Explicit Reusable Modules

**Files:**
- Modify: `workflow/include/workflow/nodeflow.hpp`
- Modify: `workflow/src/nodeflow.cpp`
- Create: `workflow/tests/test_modules.cpp`
- Modify: `workflow/CMakeLists.txt`

**Interfaces:**
- Produces: `RunContext`, `RunStatus`, `OutputPort`, `OutputBindings`, `SubflowOptions`, and `SubflowModule`.
- Produces: explicit-output overloads of `create_subgraph` and `create_subtask` whose builder returns `OutputBindings`.

- [x] **Step 1: Add failing module tests**

Cover two inputs/two outputs, a missing output binding, wrong `std::any` type at the consumer, ten dynamic invocations, two module instances on one executor, and parent/child/grandchild context propagation.

- [x] **Step 2: Implement module execution**

Each invocation creates a fresh nested `GraphBuilder`, derives `parent_run_id/subtask_id/attempt/depth`, executes with the parent executor, and resolves every declared output binding after `corun` completes.

- [x] **Step 3: Enforce contracts**

Reject missing/duplicate bindings, depth overflow, unavailable executor, and undeclared outputs with descriptive exceptions. Change legacy keyed placeholder overloads to fail explicitly until callers migrate.

- [x] **Step 4: Run module tests**

Expected: all module cases pass and no empty placeholder output remains in `workflow/src/nodeflow.cpp`.

### Task 3: Taskflow 4.x Native Conditional Loop Runtime

**Files:**
- Modify: `workflow/include/workflow/nodeflow.hpp`
- Modify: `workflow/src/nodeflow.cpp`
- Create: `workflow/tests/test_control_flow.cpp`
- Modify: `workflow/CMakeLists.txt`

**Interfaces:**
- Produces: `LoopDecision`, `LoopStatus`, `IterationContext`, `LoopOptions`, `LoopResult`, and `GraphBuilder::create_loop`.
- Consumes: reusable slots and `RunContext` from Tasks 1-2.

- [x] **Step 1: Add failing cardinality and dataflow tests**

Cover 0, 1, 2, 10, and 1000 body calls; assert `state[n+1] = state[n] + 1`; assert entry and exit execute once; run the graph three times.

- [x] **Step 2: Implement the conditional topology**

For each outer invocation create `entry -> condition`, `condition -> body/exit`, and `body -> condition`. The first condition selects body without evaluating the user condition; later conditions consume the committed body result. External predecessors only target the outer loop task.

- [x] **Step 3: Implement terminal states**

Map completed, max-iterations, cancelled, deadline-exceeded, body-error, condition-error, and exit-error to `LoopResult`. Publish user outputs and reserved status/error/iteration keys only after the nested topology reaches exit.

- [x] **Step 4: Add cancellation, exception, and concurrency tests**

Cover each terminal state, two different loop graphs concurrently on one executor, and nested module cancellation/deadline propagation.

- [x] **Step 5: Remove legacy debug state**

Delete static call counters, direct debug `std::cout`, and comments that depend on Taskflow join-counter internals. Delegate legacy loop overloads where signatures permit and document the rest as legacy.

### Task 4: AgentLoop Explicit State and Session Commit Modes

**Files:**
- Modify: `agent_framework/include/node/agent_loop_node.hpp`
- Modify: `agent_framework/src/node/agent_loop_node.cpp`
- Modify: `agent_framework/include/agent/graph_executor.hpp`
- Modify: `agent_framework/src/graph_executor/graph_executor.cpp`
- Create: `agent_framework/tests/test_agent_control_flow.cpp`
- Modify: `agent_framework/tests/test_wp20_session_merge.cpp`
- Modify: `agent_framework/CMakeLists.txt`

**Interfaces:**
- Agent loop body consumes `agent_state` and returns `next_agent_state`, `is_final`, `final_answer`, and `llm_output` each iteration.
- Produces: `MergeReactSessionMode::ResumeFromCheckpoint` and an attempt-aware merge contract.

- [x] **Step 1: Add offline mock Agent loop tests**

Use a mock LLM that emits three tool-call rounds and then a final response. Assert history, iteration, tool call identities, final output, two concurrent-session isolation, and one-session cancellation.

- [x] **Step 2: Migrate AgentLoopNode**

Remove mutable `Shared` closure state. Clone or advance the state received in the current iteration, return it in the body output, and configure loop feedback `next_agent_state -> agent_state`. Condition reads only body outputs and `IterationContext`.

- [x] **Step 3: Implement checkpoint resume merge**

Add `ResumeFromCheckpoint`; validate the checkpoint prefix and append only the uncommitted delta. Preserve exactly-once user/assistant/tool messages and iteration values.

- [x] **Step 4: Run focused Agent tests**

Expected: existing Agent loop/session tests and new `agent_control_flow` test pass offline.

### Task 5: Controlled Subflow/Sub-agent API

**Files:**
- Create: `agent_framework/include/node/subflow_node.hpp`
- Create: `agent_framework/src/node/subflow_node.cpp`
- Create: `agent_framework/tests/test_subflow_node.cpp`
- Modify: `agent_framework/CMakeLists.txt`

**Interfaces:**
- Produces: `SubflowRequest`, `SubflowResult`, `SubflowLimits`, and `SubflowNode::create` for statically registered subflow templates.

- [x] **Step 1: Add tests for bounded nesting and aggregation**

Run two child subflows, deterministically aggregate success/error results, reject excess depth/budget, and prove one failure does not discard another committed result.

- [x] **Step 2: Implement SubflowNode**

Pass parent context, cancellation, deadline, iteration/tool budgets, trace identifiers, and structured result/error/usage through Workflow module ports. Do not expose arbitrary runtime graph generation.

- [x] **Step 3: Run focused tests**

Expected: `agent_subflow` passes success, partial failure, cancellation, and budget cases.

### Task 6: Documentation and Completion Audit

**Files:**
- Modify: `workflow/README.md`
- Modify: `workflow/KEY_BASED_API.md`
- Modify: `workflow/include/workflow/nodeflow.hpp`
- Modify: `agent_framework/docs/guides/phase-1-wp5.md`
- Modify: `docs/upstream-upgrade.md`
- Modify: `docs/workflow-agent-control-flow-upgrade.md`

**Interfaces:**
- Consumes: final APIs and test names from Tasks 1-5.
- Produces: one recommended loop API, static/dynamic module comparison, lifecycle/state diagrams, and recorded verification evidence.

- [x] **Step 1: Update Workflow documentation**

Document loop entry/back-edge timing, output commit, terminal states, sequential re-run, concurrent-run rejection, static module versus dynamic subtask, explicit output bindings, nested context, and legacy API status.

- [x] **Step 2: Update Agent documentation**

Replace closure-state guidance with explicit loop feedback; add retry/restart/resume, cancellation, parallel session, and bounded sub-agent state transitions.

- [x] **Step 3: Configure and build all CPU targets**

Run: `cmake -S . -B build-control-flow -DCMAKE_BUILD_TYPE=Debug -DTF_BUILD_TESTS=ON -DTF_BUILD_EXAMPLES=OFF -DTF_BUILD_WORKFLOW=ON -DTF_BUILD_AGENT_FRAMEWORK=ON -DTF_BUILD_CUDA=OFF && cmake --build build-control-flow -j2`
Expected: successful build with no new warnings in modified Workflow/Agent sources.

- [x] **Step 4: Run semantic labels and full offline CTest**

Run: `ctest --test-dir build-control-flow -L 'workflow-control-flow|workflow-module|agent-loop|agent-subflow' --output-on-failure` and `ctest --test-dir build-control-flow --output-on-failure`.
Expected: all tests pass; live credential-dependent tests report their documented skip behavior.

- [x] **Step 5: Run static completion checks**

Run: `rg -n 'For now, return empty|Placeholder|shared state \(ignores|static int (call_count|cond_call_count)' workflow agent_framework` and `git diff --check`.
Expected: no placeholder/debug control-flow implementation remains and no whitespace errors exist.

- [x] **Step 6: Record exact evidence**

Update both upgrade documents with build commit, commands, test totals, labels, skips, and any platform limitation. Mark checklist items complete only when the corresponding test evidence exists.
