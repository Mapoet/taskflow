# AT8–AT9 Integration and Certification Matrix

Status legend: `[ ]` not exercised, `[~]` implemented but certification incomplete, `[x]` certified by recorded test.

| ID | Requirement | Status | Evidence |
|---|---|---:|---|
| AT8.1 | Five demos use one LiveRuntime production composition boundary | `[x]` | five demo targets build; shared `LiveRuntime::composition_report`; production remains fail-closed without deployment harness |
| AT8.2 | Production runner dependencies and fail-closed capability reporting | `[x]` | production-origin runner registry rejects callbacks; `/ui/bootstrap.composition` exposes dependency/readiness fields and fail-closed execution path |
| AT8.3 | Legacy alias migration diagnostics and strict mode | `[x]` | `toolbus_wp2`: canonical diagnostic, no permission expansion, strict rejection |
| AT8.4 | Unmodified Claude/Cursor/Codex Skill fixtures | `[x]` | `skill_registry_wp18`: Agents/Claude/Cursor/Codex roots and precedence |
| AT8.5 | Skill→tools→test→WebFetch→assurance→closure | `[x]` | `at8_portable_e2e`: unmodified Claude Skill, canonical Write/Edit/Bash/CMake/Make/Read/WebFetch, durable HITL restart/resume, exactly-once side effect and TaskClosure verification |
| AT9.1 | Contract and functional certification | `[x]` | canonical tools, Skill and AgentTemplate targeted suites |
| AT9.2 | Security certification | `[x]` | approval/policy, memory governance, Skill signature negative, MCP OAuth and production approval suites passed |
| AT9.3 | Lifecycle and concurrency certification | `[x]` | durable run, tool runtime, recovery certification, MCP lifecycle and SSE backpressure/replay suites passed; external multi-node chaos remains out of scope |
| AT9.4 | Correlated observability certification | `[x]` | runtime observability, store-backed operations and live operations projection suites passed |
| AT9.5 | Five-end real runtime and screenshot certification | `[x]` | Web/TUI/ImGui real screenshots, CLI and live server smoke; Web SSE now has bounded replay, independent subscriber cursors and Last-Event-ID recovery |
| AT9.6 | Regression gate and residual report | `[x]` | AT8/AT9 targeted gate 19/19, Web static, real loopback Web tool test, five-demo build and diff check passed |

Every certification entry must record the exact command, result, implementation revision and residual limitation. External PostgreSQL multi-node chaos remains explicitly outside this AT8–AT9 batch and cannot be reported as passed.

## 2026-08-14 evidence

- Five demo targets compiled: `web_ui_demo`, `tui_agent_demo`, `imgui_agent_demo`, `agent_server_demo`, `cli_agent_demo`.
- Targeted AT8/AT9 regression initially passed 19/20 in the restricted sandbox; `web_tools_builtin` could not bind loopback. The unchanged binary was rerun outside the network sandbox and passed, including SearXNG result-content enrichment, binary fetch, RSS and ZIP extraction.
- Web real runtime exposed stale `CONNECTING / Indexed 0` when SSE was unavailable. Added `/ui/bootstrap` and fetch-first initialization. Recheck showed `Indexed 162`, generation 1 and 15 real diagnostics. Screenshot: `/tmp/at9-web-fixed.png`.
- TUI real runtime displayed canonical Phase 4 blocker, residual risk, HITL and 162 Skills. Screenshot: `/tmp/at9-tui.png`.
- ImGui real runtime displayed 162 Skills and the persisted explainable failure `model_turn_cannot_verify_task`. Screenshot: `/tmp/at9-imgui.png`; successful end-to-end model task remains unexercised.
- CLI `--demo-state` displayed closure authority, blocker, HITL and child-agent Skill receipt.
- Live-only Agent Server started on loopback. Agent Card was public as designed; unauthenticated JSON-RPC returned 401 and authenticated unknown method returned a valid JSON-RPC `-32601` response.
- Final targeted regression gate passed 19/19; `test_web_ui_static.sh` and `git diff --check` passed. This is the scoped AT8/AT9 gate, not a claim that every repository test or external certification ran.
- `test_at8_portable_e2e` passed outside the restricted network sandbox. One pinned, unmodified `.claude/skills/portable-build/SKILL.md` drove canonical file/process/build/Web tools, suspended on durable approval, reopened both SQLite stores, resumed from the integrity-bound checkpoint without repeating the pre-approval side effect, fetched loopback evidence, and received `CompletedVerified` from `TaskClosureController`.
- The integrated fixture exposed and closed a false-success path: `ToolBusSkillRunner` now fails structured tool errors, timeouts and non-zero process exits. `test_agent_template_runner_compiler` covers the `tool_exit_nonzero` receipt contract.
- AgentTemplate input mapping now supports deterministic literal values for heterogeneous typed tool nodes and rejects ambiguous literal-plus-`from/path` mappings as `input_mapping_failed`.
- AT9 security/lifecycle/observability gate passed 17/17: durable run, ApprovalStore, runtime observability, tool runtime, recovery certification, production approval, approval executor, memory approval governance, store-backed/live operations, OAuth SSE, MCP lifecycle, A2A SSE framing/backpressure/contract, Skill signature security and Skill lifecycle.
- Web SSE no longer rejects a second browser or destructively consumes a global queue. Two concurrent real clients independently received the same retained stream from `id: 1`; a reconnect with `Last-Event-ID: 2` resumed at `id: 3`. `ui_dispatch_message_wpu_u1` covers independent cursors and replay. Real UI screenshot after the change: `/tmp/at9-web-sse-replay.png`.

## Residuals

- AT8.5 is certified against a real loopback HTTP endpoint to keep the test deterministic and independent of public-network availability; a public external URL is intentionally not a CI requirement.
- Web SSE retains the latest 4096 events in process memory. Durable cross-process replay remains the responsibility of the canonical event/operations stores; the HTTP presentation cache itself is not a durable store.
- Production harness dependencies are deployment-owned. Demo composition reports them unavailable rather than injecting callbacks.
- External PostgreSQL multi-node chaos and live-provider production certification remain not exercised.
