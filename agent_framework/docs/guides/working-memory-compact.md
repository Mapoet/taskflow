# Working memory compaction (WP2.9)

This document summarizes runtime behavior implemented for **phase-2-wp9** / **D10**. Authoritative detail remains in [phase-2-wp9.md](./phase-2-wp9.md).

## Single entry point

All compaction strategies go through `run_memory_compaction` in `memory_compaction.hpp`. The CLI/control path (`memory.compact` via `ControlAction`) and automatic triggering after each loop iteration both call this function (manual uses `MemoryCompactTrigger::manual_compact`; auto uses `auto_threshold` or `hard_cap`).

## Default thresholds (early trigger)

- `AGENT_MEMORY_COMPACT_TRIGGER_RATIO` defaults to **0.5**, aligned with [agents/memory.md](../agents/memory.md) §5.1: compaction tends to run when `history_utf8_bytes` reaches half of the soft limit.

## Modes

- **`truncate`** (default): keep head/tail message counts, replace the removed middle with one `system` marker line (see wp9 §5.1).
- **`summarize`**: LLM middle summary; on failure or timeout, **same** head/tail **truncate** is used (`strategy_used` becomes `fallback_truncate`).

## Hard cap and WP2.1c

If history still exceeds `AGENT_MEMORY_HARD_LIMIT_BYTES` after the primary strategy, the implementation reuses **`apply_per_tool_result_budget`** on `tool` messages (then trims long `assistant`/`user` content), emitting `_af_truncation` metadata per [phase-2-wp1c.md](./phase-2-wp1c.md).

## Metrics

`working_memory_metrics(const AgentThreadState&)` returns JSON with the fixed key set in wp9 §2.2 (`history_utf8_bytes`, `would_auto_trigger`, limits, etc.).

## Control commands

- **`memory.clear`**: clears `history` and related session fields per wp9 §4.2; does **not** clear `initial_user_prompt`.
- **`memory.compact`**: runs `run_memory_compaction` with **manual** trigger (not subject to auto throttle between compactions).

## Environment variables

See wp9 §3 for full table (`AGENT_MEMORY_SOFT_LIMIT_BYTES`, `AGENT_MEMORY_HARD_LIMIT_BYTES`, `AGENT_MEMORY_AUTO_COMPACT`, `AGENT_MEMORY_AUTO_MIN_STEPS`, `AGENT_MEMORY_COMPACT_HEAD_KEEP`, `AGENT_MEMORY_COMPACT_TAIL_KEEP`, summarize caps/timeouts, etc.).
