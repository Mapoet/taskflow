# A2A fixtures (WP2.6)

## Layout

- `synthetic-v1/` — hand-written bundle aligned with [a2a-spec-tracker.md](../../docs/guides/a2a-spec-tracker.md).
- Each bundle has `manifest.json` listing files, assertion kind, and `applicable_tiers` (`A`, `A+B`, …).
- Legacy flat files `card_min.json`, `task_min_v1.json` remain for existing unit tests.

## Updating goldens

1. Run `ctest -R a2a_contract` (or the specific failing test).
2. If the wire change is intentional, set `AGENT_A2A_UPDATE_GOLDENS=1` and run `a2a_fixture_regen` (see CMake target); review diffs.
3. Update `tracker_revision` in `manifest.json` when the tracker changes.

## CI

Default jobs are offline: no `curl` to the public internet. Do not enable `AGENT_A2A_UPDATE_GOLDENS` in PR CI.
