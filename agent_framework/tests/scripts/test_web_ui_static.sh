#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
HTML="${ROOT}/examples/web_ui_static/index.html"
CSS="${ROOT}/examples/web_ui_static/styles.css"
JS="${ROOT}/examples/web_ui_static/app.js"
CPP="${ROOT}/examples/web_ui_demo.cpp"
MARKDOWN_IT="${ROOT}/examples/web_ui_static/vendor/markdown-it/markdown-it.min.js"
DOMPURIFY="${ROOT}/examples/web_ui_static/vendor/dompurify/purify.min.js"
KATEX="${ROOT}/examples/web_ui_static/vendor/katex/katex.min.js"
MERMAID="${ROOT}/examples/web_ui_static/vendor/mermaid/mermaid.min.js"

for file in "${HTML}" "${CSS}" "${JS}" "${CPP}" "${MARKDOWN_IT}" "${DOMPURIFY}" "${KATEX}" "${MERMAID}"; do
    [[ -s "${file}" ]] || { printf 'missing: %s\n' "${file}" >&2; exit 1; }
done

grep -Fq 'class="topbar"' "${HTML}"
grep -Fq 'class="conversation-panel"' "${HTML}"
grep -Fq 'class="activity-panel"' "${HTML}"
grep -Fq 'class="composer"' "${HTML}"
grep -Fq 'styles.css' "${HTML}"
grep -Fq 'Content-Security-Policy' "${HTML}"
grep -Fq 'vendor/markdown-it/markdown-it.min.js' "${HTML}"
grep -Fq 'vendor/dompurify/purify.min.js' "${HTML}"
grep -Fq 'vendor/katex/katex.min.js' "${HTML}"
grep -Fq 'vendor/mermaid/mermaid.min.js' "${HTML}"
grep -Fq '@media (max-width: 760px)' "${CSS}"
grep -Fq 'prefers-reduced-motion' "${CSS}"
grep -Fq 'id="operations-view"' "${HTML}"
grep -Fq 'id="ops-memory"' "${HTML}"
grep -Fq 'id="ops-invocations"' "${HTML}"
grep -Fq 'id="ops-assurance"' "${HTML}"
grep -Fq 'id="ops-hitl"' "${HTML}"
grep -Fq 'textContent' "${JS}"
grep -Fq 'DOMPurify.sanitize' "${JS}"
grep -Fq 'new DOMParser()' "${JS}"
grep -Fq 'window.setTimeout(run, 72)' "${JS}"
grep -Fq 'o.kind === "thinking"' "${JS}"
grep -Fq 'renderMermaid' "${JS}"
grep -Fq 'shiftKey' "${JS}"
grep -Fq '/ui/cancel' "${JS}"
grep -Fq 'type === "mcp_status"' "${JS}"
grep -Fq 'id="skills-state"' "${HTML}"
grep -Fq 'id="skills-root"' "${HTML}"
grep -Fq 'type === "skills_status"' "${JS}"
grep -Fq 'type === "phase4_operations"' "${JS}"
grep -Fq 'renderOperations' "${JS}"
grep -Fq '/ui/operations/hitl' "${JS}"
grep -Fq '/ui/operations/snapshot' "${JS}"
grep -Fq 'svr.Post("/ui/cancel"' "${CPP}"
grep -Fq 'svr.Post("/ui/operations/hitl"' "${CPP}"
grep -Fq 'svr.Get("/ui/operations/snapshot"' "${CPP}"
grep -Fq 'svr.Get("/ui/interactions/snapshot"' "${CPP}"
grep -Fq 'svr.Get("/ui/interactions/events"' "${CPP}"
grep -Fq '/ui/interactions/node/' "${CPP}"
grep -Fq 'SQLiteInteractionProjectionStore' "${CPP}"
grep -Fq '"mcp_status"' "${CPP}"
grep -Fq '"skills_status"' "${CPP}"
grep -Fq -- '--skills-root' "${CPP}"
grep -Fq ': heartbeat' "${CPP}"
grep -Fq 'sink.is_writable()' "${CPP}"
grep -Fq 'tool_execution_observer' "${CPP}"
grep -Fq 'svr.Get(R"(/ui/files/(.*))"' "${CPP}"
grep -Fq 'fs_resolve_under_root' "${CPP}"
grep -Fq 'X-Content-Type-Options' "${CPP}"

if grep -Eq 'innerHTML|insertAdjacentHTML|document\.write' "${JS}"; then
    printf 'unsafe HTML insertion found\n' >&2
    exit 1
fi

printf 'test_web_ui_static: ok\n'
