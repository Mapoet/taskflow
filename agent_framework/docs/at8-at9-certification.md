# AT8–AT9 Integration and Certification Matrix

Status legend: `[ ]` not exercised, `[~]` implemented but certification incomplete, `[x]` certified by recorded test.

| ID | Requirement | Status | Evidence |
|---|---|---:|---|
| AT8.1 | Five demos use one LiveRuntime production composition boundary | `[x]` | five demo targets build; shared `LiveRuntime::composition_report`; production remains fail-closed without deployment harness |
| AT8.2 | Production runner dependencies and fail-closed capability reporting | `[~]` | AgentTemplate production runner tests; UI/API exposure pending |
| AT8.3 | Legacy alias migration diagnostics and strict mode | `[x]` | `toolbus_wp2`: canonical diagnostic, no permission expansion, strict rejection |
| AT8.4 | Unmodified Claude/Cursor/Codex Skill fixtures | `[x]` | `skill_registry_wp18`: Agents/Claude/Cursor/Codex roots and precedence |
| AT8.5 | Skill→tools→test→WebFetch→assurance→closure | `[ ]` | end-to-end fixture pending |
| AT9.1 | Contract and functional certification | `[x]` | canonical tools, Skill and AgentTemplate targeted suites |
| AT9.2 | Security certification | `[~]` | targeted negative tests exist; matrix consolidation pending |
| AT9.3 | Lifecycle and concurrency certification | `[~]` | durable resume/CAS exists; complete race matrix pending |
| AT9.4 | Correlated observability certification | `[~]` | LLM/tool/LTW projection tests exist |
| AT9.5 | Five-end real runtime and screenshot certification | `[~]` | Web/TUI/ImGui real screenshots; CLI and live server smoke; Web SSE single-consumer reconnect remains conditional |
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

## Residuals

- AT8.5 full portable workflow with a real external WebFetch and assurance closure is not yet certified as one uninterrupted scenario.
- Web SSE is intentionally single-consumer and can temporarily report reconnecting after an abnormal client lifecycle. Bootstrap and operations snapshots now remain correct independently of SSE.
- Production harness dependencies are deployment-owned. Demo composition reports them unavailable rather than injecting callbacks.
- External PostgreSQL multi-node chaos and live-provider production certification remain not exercised.
