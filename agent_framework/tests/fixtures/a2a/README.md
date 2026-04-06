# A2A fixtures (WP2.6)

## Layout

- `synthetic-v1/` — hand-written bundle aligned with [a2a-spec-tracker.md](../../docs/guides/a2a-spec-tracker.md).
- Each bundle has `manifest.json` listing files, assertion kind, and `applicable_tiers` (`A`, `A+B`, …).
- Legacy flat files `card_min.json`, `task_min_v1.json` remain for existing unit tests.

## Manifest `kind` / `assert` → test behavior

| `kind` | `assert` (and aliases) | Meaning |
|--------|------------------------|---------|
| `jsonrpc_request` | `parse_ok` | File is JSON; `dump()` → `parse_jsonrpc_request` yields `JsonRpcRequest`. |
| `jsonrpc_request_raw` | `expect_jsonrpc_error_response` | **UTF-8 text** file (not necessarily valid JSON); body passed to `parse_jsonrpc_request`; result must be error envelope; optional `expected_error_code` (e.g. `-32700`). |
| `jsonrpc_request` | `expect_jsonrpc_error_response` | File is JSON; `dump()` → parser must return error; optional `expected_error_code` (e.g. `-32600` for batch / invalid request). |
| `jsonrpc_response` | `jsonrpc_envelope_ok` | Has `jsonrpc` 2.0 and `result`. |
| `jsonrpc_response` | `jsonrpc_error_ok` | Has `error` object with `code`. |
| `agent_card_json` | `round_trip_types` | Card wire round-trip (skills non-empty for `card_with_skills`). |
| `task_wire` | `round_trip_types` | `task_from_a2a_wire` → `task_to_a2a_wire`; canonical compare with `ignore_json_keys`. If `equals_after_parse` is set, **also** canonical-compare `path` JSON to that file (same ignores). |
| `sse_stream` | `sse_parse_ok` \| **`sse_event_count`** (alias) | Parse SSE file; at least `expected_min_events`; each `data` line parses as JSON; optional **`expected_event_names`** in order. |

[phase-2-wp6.md](../../docs/guides/phase-2-wp6.md) §2.2 uses names like `equals_canonical_json_after_parse` / `sse_event_count`; in this repo the **implemented** primary names are in the table above; `sse_event_count` is accepted as an alias of `sse_parse_ok` for SSE fixtures.

## `equals_after_parse` (optional, `task_wire`)

- If **omitted**: only round-trip (serialize after deserialize) is checked.
- If **set**: relative path under the same bundle; the JSON at `path` must canonically match that file after `ignore_json_keys` (used for dual-golden / cross-check).

## Canonical JSON (comparison)

- Equality uses `nlohmann::json` parse → `dump()` on both sides after optional key stripping; **not** raw byte-for-byte of files.
- Do not rely on `dump()` stability across library upgrades; intentional wire changes → update goldens and `tracker_revision`.

## Updating goldens

1. Run `ctest -R a2a_contract` (or the specific failing test).
2. If the wire change is intentional, set `AGENT_A2A_UPDATE_GOLDENS=1` and run `a2a_fixture_regen` with `--in` / `--out` (see tool `--help`); review diffs.
3. Without env, you may redirect stdout: `AGENT_A2A_UPDATE_GOLDENS=1 ./a2a_fixture_regen --in ... --out ...`
4. Update `tracker_revision` in `manifest.json` when the tracker changes.

## CI

Default jobs are offline: no `curl` to the public internet. Do not enable `AGENT_A2A_UPDATE_GOLDENS` in PR CI.

M3 contract gate includes `a2a_contract_json` and `a2a_contract_sse` (see [phase-2-plan.md](../../docs/guides/phase-2-plan.md) §5).

### `a2a_contract_loopback` (L-2 optional)

- **L-1** runs always: mock server returns [send_message_response.json](synthetic-v1/jsonrpc/send_message_response.json).
- **L-2** runs when `AGENT_A2A_CONTRACT_TEST_BEARER` is set to a non-empty test token (e.g. local only); verifies 401 without `Authorization` and success with `Bearer` token.
