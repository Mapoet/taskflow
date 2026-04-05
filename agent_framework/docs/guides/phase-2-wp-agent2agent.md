# WP2.agent2agent — A2A orchestration library + CLI demo

**ID**: WP2.agent2agent (orchestration only; reuses JSON-RPC / Card / `AgentClient` / SSE).  
**Version**: 0.1 · **Date**: 2026-04-05  

**Wire authority**: [a2a-spec-tracker.md](./a2a-spec-tracker.md), [agent-client.md](./agent-client.md), `include/agent/a2a/client_config.hpp`.

## Goals (DoD)

- **G1** Public API in `agent_framework::a2a` (`peer_registry.hpp`, `orchestration.hpp`).
- **G2** `peers.json` parse errors → `std::invalid_argument` with field hints; duplicate `id` rejected.
- **G3** `ToolBus`: `a2a.send_message` (+ optional `a2a.send_message__<peer_id>`), `ToolSideEffect::Write`.
- **G4** Wait: streaming capability → SSE latch; else `GetTask` poll; `A2aRpcException::code` → `a2a_error_code` in JSON.
- **G5** `PeerSessionBook` updated when terminal task carries `session_id` / `contextId`.
- **G6** Remote log lines: default `stderr` `[a2a:<peer_id>]`; `--merge-streams` merges into LLM stream callback in demo.
- **G7** Binary `cli_a2a_orchestrator_demo` + `examples/configs/a2a_peers.example.json`.
- **G8** Tests `test_a2a_peer_registry`, `test_a2a_orchestrator_tools` (loopback `AgentServer`, no external net).

User guide: **[a2a-orchestrator.md](./a2a-orchestrator.md)**. Tier C/D live tests remain in **[a2a-integration-tests.md](./a2a-integration-tests.md)**.

## Public API (`agent_framework::a2a`)

| API | Role |
|-----|------|
| `split_json_rpc_url` | Split Card `api_endpoint` → origin + path |
| `resolve_peer_auth_config` | Resolve `token_env` from environment |
| `make_rpc_agent_client_for_card` | Build JSON-RPC `AgentClient` |
| `A2aPeerRegistry` | `load_from_json` / file, `discover_all`, `card`, `client`, `peer_ids`, `default_timeout_ms_for` |
| `PeerSessionBook` | Per-peer `contextId` / session |
| `run_remote_task_and_wait` | SendMessage + wait → tool JSON |
| `register_a2a_orchestrator_tools` | Register tools on `ToolBus` |
| `kA2aOrchestratorToolSendMessage` | `"a2a.send_message"` |

## peers.json (v1)

Root: `{ "peers": [ ... ] }`. Each peer: `id`, `origin` (with scheme); optional `well_known_path` (default `/.well-known/agent-card.json`), `auth`, `default_timeout_ms` (positive int).

## Scope

Flat tool calls + session table + terminal wait only; no Verifier routing, no hot MCP, no multi-hop DAG in v1.

## See also

[tool-orchestration.md](./tool-orchestration.md), [phase-2-plan.md](./phase-2-plan.md).
