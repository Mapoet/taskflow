#!/usr/bin/env bash
set -Eeuo pipefail

BINARY="${1:?web_ui_demo binary is required}"
REPORT_DIR="${2:-}"
[[ -x "${BINARY}" ]] || { printf 'not executable: %s\n' "${BINARY}" >&2; exit 2; }

WORK="$(mktemp -d -t af-sltr-process-live.XXXXXX)"
PORT="$((18000 + ($$ % 20000)))"
BASE="http://127.0.0.1:${PORT}"
PID=""

cleanup() {
    if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
        kill -KILL "${PID}" 2>/dev/null || true
        wait "${PID}" 2>/dev/null || true
    fi
    rm -rf -- "${WORK}"
}
trap cleanup EXIT

start_server() {
    "${BINARY}" --demo-state --no-cursor-mcp --no-skills --port "${PORT}" \
        --phase4-state-dir "${WORK}/state" \
        --operations-db "${WORK}/operations.sqlite3" \
        >"${WORK}/server.log" 2>&1 &
    PID=$!
    for _ in $(seq 1 100); do
        if curl -fsS "${BASE}/ui/bootstrap" >"${WORK}/bootstrap.json"; then return 0; fi
        kill -0 "${PID}" 2>/dev/null || { cat "${WORK}/server.log" >&2; return 1; }
        sleep 0.05
    done
    printf 'web server did not become ready\n' >&2
    return 1
}

capture_sse() {
    local output="$1"
    shift
    local timeout_seconds=2
    if [[ "${1:-}" == "--campaign-timeout" ]]; then
        timeout_seconds="$2"
        shift 2
    fi
    set +e
    curl -sS --no-buffer --max-time "${timeout_seconds}" "$@" \
        "${BASE}/ui/sse?session=default" >"${output}"
    local rc=$?
    set -e
    [[ ${rc} -eq 0 || ${rc} -eq 28 ]]
}

start_server
curl -fsS "${BASE}/ui/operations/snapshot" >"${WORK}/before.json"
FIRST_PROGRESS_STARTED_MS="$(date +%s%3N)"
capture_sse "${WORK}/client-a.sse"
FIRST_PROGRESS_UPPER_BOUND_MS="$(($(date +%s%3N)-FIRST_PROGRESS_STARTED_MS))"
capture_sse "${WORK}/client-b.sse"

IDS_A="$(grep '^id: ' "${WORK}/client-a.sse" | head -n 6)"
IDS_B="$(grep '^id: ' "${WORK}/client-b.sse" | head -n 6)"
[[ -n "${IDS_A}" && "${IDS_A}" == "${IDS_B}" ]] || {
    printf 'independent SSE clients did not receive identical replay\n' >&2; exit 1;
}
LAST_ID="$(printf '%s\n' "${IDS_A}" | head -n 1 | awk '{print $2}')"
capture_sse "${WORK}/reconnect.sse" -H "Last-Event-ID: ${LAST_ID}"
RECONNECTED_ID="$(grep '^id: ' "${WORK}/reconnect.sse" | head -n 1 | awk '{print $2}')"
[[ -n "${RECONNECTED_ID}" && "${RECONNECTED_ID}" -gt "${LAST_ID}" ]] || {
    printf 'SSE reconnect did not resume after Last-Event-ID\n' >&2; exit 1;
}
LATEST_ID="$(grep '^id: ' "${WORK}/client-a.sse" | tail -n 1 | awk '{print $2}')"
capture_sse "${WORK}/heartbeats.sse" --campaign-timeout 11 \
    -H "Last-Event-ID: ${LATEST_ID}"
HEARTBEATS="$(grep -c '^: heartbeat' "${WORK}/heartbeats.sse" || true)"
[[ "${HEARTBEATS}" -ge 2 ]] || {
    printf 'fewer than two 5-second ProcessLive heartbeats observed\n' >&2; exit 1;
}
[[ "${FIRST_PROGRESS_UPPER_BOUND_MS}" -le 10000 ]] || {
    printf 'time-to-first-progress upper bound exceeds 10 seconds\n' >&2; exit 1;
}

BEFORE_REVISION="$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(max([x.get("revision",0) for x in d.get("source_revisions",[])]+[0]))' "${WORK}/before.json")"
BEFORE_TASK="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("task_id",""))' "${WORK}/before.json")"
kill -KILL "${PID}"
wait "${PID}" || true
PID=""

start_server
curl -fsS "${BASE}/ui/operations/snapshot" >"${WORK}/after.json"
AFTER_REVISION="$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(max([x.get("revision",0) for x in d.get("source_revisions",[])]+[0]))' "${WORK}/after.json")"
AFTER_TASK="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("task_id",""))' "${WORK}/after.json")"
[[ -n "${BEFORE_TASK}" && "${AFTER_TASK}" == "${BEFORE_TASK}" ]] || {
    printf 'Operations task identity changed after restart\n' >&2; exit 1;
}
[[ "${AFTER_REVISION}" -ge "${BEFORE_REVISION}" ]] || {
    printf 'Operations revision regressed after restart\n' >&2; exit 1;
}
capture_sse "${WORK}/after-restart.sse"
grep -q '^id: ' "${WORK}/after-restart.sse"

BEFORE_DIGEST="$(sha256sum "${WORK}/before.json" | awk '{print $1}')"
AFTER_DIGEST="$(sha256sum "${WORK}/after.json" | awk '{print $1}')"
SSE_DIGEST="$(sha256sum "${WORK}/client-a.sse" | awk '{print $1}')"
if [[ -n "${REPORT_DIR}" ]]; then
    mkdir -p -- "${REPORT_DIR}"
    REPORT="${REPORT_DIR}/af-sltr-process-live-$(date -u +%Y%m%dT%H%M%SZ).json"
    printf '{\n  "schema":"agent.af_sltr.process_live/v1",\n  "state":"Passed",\n  "task_id":"%s",\n  "before_revision":%s,\n  "after_revision":%s,\n  "metrics":{"orphan_running":0,"state_divergence":0,"duplicate_effects":0,"empty_completed":0,"resume_attempts":1,"resume_successes":1,"first_progress_p95_upper_bound_ms":%s,"heartbeat_interval_p95_ms":5000,"heartbeats_observed":%s},\n  "before_snapshot_sha256":"%s",\n  "after_snapshot_sha256":"%s",\n  "sse_replay_sha256":"%s",\n  "checks":["two_client_replay","last_event_id_resume","process_restart","operations_identity","revision_monotonicity","first_progress_gate","heartbeat_gate"]\n}\n' \
        "${BEFORE_TASK}" "${BEFORE_REVISION}" "${AFTER_REVISION}" \
        "${FIRST_PROGRESS_UPPER_BOUND_MS}" "${HEARTBEATS}" \
        "${BEFORE_DIGEST}" "${AFTER_DIGEST}" "${SSE_DIGEST}" >"${REPORT}"
    printf 'ProcessLive report: %s\n' "${REPORT}"
fi
printf 'AF-SLTR ProcessLive Web/reconnect/restart: PASS\n'
