#!/usr/bin/env bash
set -Eeuo pipefail

BINARY="${1:?workbench_runtime_server binary is required}"
CHROME="${2:?Chrome binary is required}"
WORK="$(mktemp -d -t af-workbench-browser.XXXXXX)"
PORT="$((22000 + ($$ % 18000)))"
BASE="http://127.0.0.1:${PORT}"
PID=""
cleanup() {
  if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
    kill -TERM "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
  fi
  rm -rf -- "${WORK}"
}
trap cleanup EXIT

"${BINARY}" "${PORT}" >"${WORK}/server.log" 2>&1 & PID=$!
for _ in $(seq 1 120); do
  curl -fsS "${BASE}/api/v1/sessions?limit=10" >/dev/null && break
  kill -0 "${PID}" 2>/dev/null || { cat "${WORK}/server.log" >&2; exit 1; }
  sleep .05
done

HEADERS=(-H 'Content-Type: application/json' -H 'X-Agent-Tenant: local'
  -H 'X-Agent-Organization: local' -H 'X-Agent-Project: local'
  -H 'X-Agent-Workspace: local' -H 'X-Agent-Principal: local-user')
render() {
  "${CHROME}" --headless=new --no-sandbox --disable-gpu --virtual-time-budget=6000 \
    --user-data-dir="${WORK}/chrome-profile" \
    --dump-dom "$1" 2>"${WORK}/chrome.err"
}

render "${BASE}/#session-orbital" >"${WORK}/orbital-before.html"
grep -Fq 'Orbital analysis and runtime closure' "${WORK}/orbital-before.html"
grep -Fq 'How deeply should the orbital workflow be verified?' "${WORK}/orbital-before.html"
grep -Fq 'Five-layer governed context assembled' "${WORK}/orbital-before.html"
grep -Fq 'Repository inventory synchronized' "${WORK}/orbital-before.html"
grep -Fq 'Orbital verification report attached' "${WORK}/orbital-before.html"
grep -Fq '<h1>Orbital verification</h1>' "${WORK}/orbital-before.html"
grep -Fq '<table>' "${WORK}/orbital-before.html"
grep -Fq 'class="katex"' "${WORK}/orbital-before.html"
grep -Fq 'class="mermaid-block"' "${WORK}/orbital-before.html"
! grep -Fq '<script id="markdown-xss-canary"' "${WORK}/orbital-before.html"
grep -Fq 'New Session' "${WORK}/orbital-before.html"
grep -Fq 'System settings' "${WORK}/orbital-before.html"
grep -Fq 'local-user' "${WORK}/orbital-before.html"
! grep -Fq 'ISOLATED MEMORY CANARY' "${WORK}/orbital-before.html"
! grep -Fq 'ISOLATED TOOL CANARY' "${WORK}/orbital-before.html"
! grep -Fq 'ISOLATED ARTIFACT CANARY' "${WORK}/orbital-before.html"

curl -fsS "${HEADERS[@]}" -d '{"expected_revision":1,"option_id":"professional"}' \
  "${BASE}/api/v1/sessions/session-orbital/decisions/scope-decision/answer" \
  >"${WORK}/answer.json"
python3 - "${WORK}/answer.json" <<'PY'
import json,sys
value=json.load(open(sys.argv[1],encoding='utf-8'))
assert value['answered'] is True and value['revision']==2 and value['state']=='answered'
PY
render "${BASE}/#session-orbital" >"${WORK}/orbital-after.html"
grep -Fq 'Decision answered' "${WORK}/orbital-after.html"
grep -Fq 'selected' "${WORK}/orbital-after.html"
grep -Fq 'Run completed' "${WORK}/orbital-after.html"

render "${BASE}/#session-isolated" >"${WORK}/isolated.html"
grep -Fq 'ISOLATED MEMORY CANARY' "${WORK}/isolated.html"
grep -Fq 'ISOLATED TOOL CANARY' "${WORK}/isolated.html"
grep -Fq 'ISOLATED ARTIFACT CANARY' "${WORK}/isolated.html"
! grep -Fq 'Five-layer governed context assembled' "${WORK}/isolated.html"
! grep -Fq 'Repository inventory synchronized' "${WORK}/isolated.html"
! grep -Fq 'Orbital verification report attached' "${WORK}/isolated.html"

render "${BASE}/?panel=settings#session-orbital" >"${WORK}/settings.html"
grep -Fq 'Revision-aware deployment configuration' "${WORK}/settings.html"
grep -Fq 'Provider / Model' "${WORK}/settings.html"
grep -Fq 'Tool sandbox' "${WORK}/settings.html"
grep -Fq 'Assurance / Judge' "${WORK}/settings.html"
grep -Fq 'Logging / Observability' "${WORK}/settings.html"

render "${BASE}/?panel=profile#session-orbital" >"${WORK}/profile.html"
grep -Fq 'Authenticated runtime identity' "${WORK}/profile.html"
grep -Fq 'Authorization revision' "${WORK}/profile.html"

printf 'AF Workbench BrowserLive decision/resume/session-isolation: PASS\n'
