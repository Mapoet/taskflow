#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
HTML="${ROOT}/examples/web_ui_static/index.html"
CSS="${ROOT}/examples/web_ui_static/styles.css"
JS="${ROOT}/examples/web_ui_static/app.js"
CPP="${ROOT}/examples/web_ui_demo.cpp"

for file in "${HTML}" "${CSS}" "${JS}" "${CPP}"; do
    [[ -s "${file}" ]] || { printf 'missing: %s\n' "${file}" >&2; exit 1; }
done

grep -Fq 'class="topbar"' "${HTML}"
grep -Fq 'class="conversation-panel"' "${HTML}"
grep -Fq 'class="activity-panel"' "${HTML}"
grep -Fq 'class="composer"' "${HTML}"
grep -Fq 'styles.css' "${HTML}"
grep -Fq '@media (max-width: 760px)' "${CSS}"
grep -Fq 'prefers-reduced-motion' "${CSS}"
grep -Fq 'textContent' "${JS}"
grep -Fq 'shiftKey' "${JS}"
grep -Fq '/ui/cancel' "${JS}"
grep -Fq 'type === "mcp_status"' "${JS}"
grep -Fq 'id="skills-state"' "${HTML}"
grep -Fq 'id="skills-root"' "${HTML}"
grep -Fq 'type === "skills_status"' "${JS}"
grep -Fq 'svr.Post("/ui/cancel"' "${CPP}"
grep -Fq '"mcp_status"' "${CPP}"
grep -Fq '"skills_status"' "${CPP}"
grep -Fq -- '--skills-root' "${CPP}"
grep -Fq ': heartbeat' "${CPP}"
grep -Fq 'sink.is_writable()' "${CPP}"
grep -Fq 'tool_execution_observer' "${CPP}"

if grep -Eq 'innerHTML|insertAdjacentHTML|document\.write' "${JS}"; then
    printf 'unsafe HTML insertion found\n' >&2
    exit 1
fi

printf 'test_web_ui_static: ok\n'
