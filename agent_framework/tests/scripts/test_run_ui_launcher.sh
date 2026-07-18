#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../../.." && pwd -P)"
LAUNCHER="${REPO_ROOT}/agent_framework/tools/run_ui.sh"
LEGACY_LAUNCHER="${REPO_ROOT}/agent_framework/tools/run_tui.sh"
TMP_ROOT="$(mktemp -d)"
trap 'rm -rf -- "${TMP_ROOT}"' EXIT
mkdir -p "${TMP_ROOT}/home" "${TMP_ROOT}/fs-root"
: >"${TMP_ROOT}/empty.env"

fail() {
    printf 'test_run_ui_launcher: %s\n' "$*" >&2
    exit 1
}

run_clean() {
    env -i \
        PATH="${PATH}" \
        HOME="${TMP_ROOT}/home" \
        TERM=xterm-256color \
        LANG=C.UTF-8 \
        "$@"
}

expect_failure() {
    local pattern="$1"
    shift
    local output
    if output="$(run_clean "$@" 2>&1)"; then
        fail "command unexpectedly succeeded: $*"
    fi
    grep -Fq -- "${pattern}" <<<"${output}" || fail "missing failure text '${pattern}' in: ${output}"
}

dry_run_ui() {
    local ui="$1"
    shift
    run_clean \
        OPENAI_API_KEY=test-key \
        "${LAUNCHER}" \
        --env-file "${TMP_ROOT}/empty.env" \
        --dry-run \
        --no-cursor-mcp \
        --ui "${ui}" \
        --build-dir "${TMP_ROOT}/build" \
        --fs-root "${TMP_ROOT}/fs-root" \
        "$@"
}

bash -n "${LAUNCHER}"
bash -n "${LEGACY_LAUNCHER}"
run_clean "${LAUNCHER}" --help | grep -Fq -- '--ui NAME' || fail "help omits --ui"

TUI_OUTPUT="$(dry_run_ui tui)"
grep -Fq -- 'interface:  tui (tui_agent_demo)' <<<"${TUI_OUTPUT}" || fail "TUI selection missing"
grep -Fq -- 'TUI backend: FTXUI 7.0.1 (vendored submodule)' <<<"${TUI_OUTPUT}" || fail "FTXUI backend missing"
grep -Fq -- '-DAGENT_BUILD_TUI=ON' <<<"${TUI_OUTPUT}" || fail "TUI CMake option missing"
grep -Fq -- '--target tui_agent_demo' <<<"${TUI_OUTPUT}" || fail "TUI build target missing"
grep -Fq -- "--skills-root ${TMP_ROOT}/home/.codex/skills" <<<"${TUI_OUTPUT}" || fail "TUI Skill root missing"

IMGUI_OUTPUT="$(dry_run_ui imgui)"
grep -Fq -- 'interface:  imgui (imgui_agent_demo)' <<<"${IMGUI_OUTPUT}" || fail "ImGui selection missing"
grep -Fq -- '-DAGENT_BUILD_IMGUI=ON' <<<"${IMGUI_OUTPUT}" || fail "ImGui CMake option missing"
grep -Fq -- '--target imgui_agent_demo' <<<"${IMGUI_OUTPUT}" || fail "ImGui build target missing"
grep -Fq -- "--skills-root ${TMP_ROOT}/home/.codex/skills" <<<"${IMGUI_OUTPUT}" || fail "ImGui Skill root missing"

WEB_OUTPUT="$(dry_run_ui web --port 9090 --demo-state --skip-mcp-service python_execute --skip-mcp-service playwright)"
grep -Fq -- 'interface:  web (web_ui_demo)' <<<"${WEB_OUTPUT}" || fail "Web selection missing"
grep -Fq -- '-DAGENT_BUILD_WEB_UI=ON' <<<"${WEB_OUTPUT}" || fail "Web CMake option missing"
grep -Fq -- '--target web_ui_demo' <<<"${WEB_OUTPUT}" || fail "Web build target missing"
grep -Fq -- "--skills-root ${TMP_ROOT}/home/.codex/skills" <<<"${WEB_OUTPUT}" || fail "Web Skill root missing"
grep -Fq -- 'web URL:     http://127.0.0.1:9090/' <<<"${WEB_OUTPUT}" || fail "Web URL missing"
grep -Fq -- '--port 9090' <<<"${WEB_OUTPUT}" || fail "Web port forwarding missing"
grep -Fq -- 'demo state: enabled' <<<"${WEB_OUTPUT}" || fail "demo-state summary missing"
grep -Fq -- '--demo-state' <<<"${WEB_OUTPUT}" || fail "demo-state forwarding missing"
grep -Fq -- 'skipped MCP: python_execute,playwright' <<<"${WEB_OUTPUT}" || fail "MCP service filter missing"

mkdir -p "${TMP_ROOT}/explicit-skills"
EXPLICIT_SKILLS_OUTPUT="$(run_clean AGENT_SKILLS_DIR="${TMP_ROOT}/explicit-skills" \
    OPENAI_API_KEY=test-key "${LAUNCHER}" --env-file "${TMP_ROOT}/empty.env" \
    --dry-run --ui web --no-cursor-mcp --build-dir "${TMP_ROOT}/build" \
    --fs-root "${TMP_ROOT}/fs-root")"
grep -Fq -- "--skills-root ${TMP_ROOT}/explicit-skills" <<<"${EXPLICIT_SKILLS_OUTPUT}" || \
    fail "AGENT_SKILLS_DIR precedence was not preserved"

NO_SKILLS_OUTPUT="$(dry_run_ui imgui --no-skills)"
grep -Fq -- 'skills root: disabled' <<<"${NO_SKILLS_OUTPUT}" || fail "disabled Skills summary missing"
grep -Fq -- '--no-skills' <<<"${NO_SKILLS_OUTPUT}" || fail "--no-skills forwarding missing"
if grep -Fq -- '--skills-root' <<<"${NO_SKILLS_OUTPUT}"; then
    fail "disabled Skills unexpectedly forwarded a root"
fi

SECRET='launcher-secret-must-not-leak'
OUTPUT="$(run_clean \
    OPENAI_API_KEY="${SECRET}" \
    "${LAUNCHER}" \
    --env-file "${TMP_ROOT}/empty.env" \
    --dry-run \
    --ui imgui \
    --no-cursor-mcp \
    --build-dir "${TMP_ROOT}/build" \
    --build-type Debug \
    --fs-root "${TMP_ROOT}/fs-root" \
    --provider openai \
    --model test-model \
    --max-iterations 7 \
    --prompt 'dry run prompt')"

grep -Fq -- "build:      ${TMP_ROOT}/build (Debug)" <<<"${OUTPUT}" || fail "build override missing"
grep -Fq -- "fs jail:    ${TMP_ROOT}/fs-root" <<<"${OUTPUT}" || fail "FS jail override missing"
grep -Fq -- 'provider:   openai' <<<"${OUTPUT}" || fail "provider missing"
grep -Fq -- 'model:      test-model' <<<"${OUTPUT}" || fail "model missing"
grep -Fq -- 'web/expr/draw: 1/1/1' <<<"${OUTPUT}" || fail "full tool defaults missing"
grep -Fq -- 'skills catalog: 1' <<<"${OUTPUT}" || fail "skills catalog default missing"
grep -Fq -- 'Cursor MCP: disabled' <<<"${OUTPUT}" || fail "MCP override missing"
grep -Fq -- 'MCP timeout: 60000 ms' <<<"${OUTPUT}" || fail "MCP timeout default missing"
grep -Fq -- 'API credential: configured (redacted)' <<<"${OUTPUT}" || fail "credential redaction marker missing"
if grep -Fq -- "${SECRET}" <<<"${OUTPUT}"; then
    fail "secret leaked in dry-run output"
fi

LEGACY_OUTPUT="$(run_clean OPENAI_API_KEY=test-key "${LEGACY_LAUNCHER}" \
    --env-file "${TMP_ROOT}/empty.env" --dry-run --no-cursor-mcp \
    --build-dir "${TMP_ROOT}/legacy" --fs-root "${TMP_ROOT}/fs-root")"
grep -Fq -- 'interface:  tui (tui_agent_demo)' <<<"${LEGACY_OUTPUT}" || fail "legacy TUI wrapper did not select TUI"

expect_failure '--ui must be tui, imgui, or web' \
    "${LAUNCHER}" --env-file "${TMP_ROOT}/empty.env" --dry-run --ui invalid --fs-root "${TMP_ROOT}/fs-root"
expect_failure '--port is only valid with --ui web' \
    "${LAUNCHER}" --env-file "${TMP_ROOT}/empty.env" --dry-run --ui imgui --port 9090 --fs-root "${TMP_ROOT}/fs-root"
expect_failure '--port must be an integer from 1 to 65535' \
    "${LAUNCHER}" --env-file "${TMP_ROOT}/empty.env" --dry-run --ui web --port 65536 --fs-root "${TMP_ROOT}/fs-root"
expect_failure '--build-type must be Release or Debug' \
    "${LAUNCHER}" --env-file "${TMP_ROOT}/empty.env" --dry-run --build-type RelWithDebInfo --fs-root "${TMP_ROOT}/fs-root"
expect_failure '--max-iterations must be a positive integer' \
    "${LAUNCHER}" --env-file "${TMP_ROOT}/empty.env" --dry-run --max-iterations 0 --fs-root "${TMP_ROOT}/fs-root"
expect_failure 'AGENT_UI_BUILD_JOBS must be a positive integer' \
    AGENT_UI_BUILD_JOBS=0 OPENAI_API_KEY=test-key \
    "${LAUNCHER}" --env-file "${TMP_ROOT}/empty.env" --dry-run --fs-root "${TMP_ROOT}/fs-root"
expect_failure 'AGENT_MCP_REQUEST_TIMEOUT_MS must be a positive integer' \
    AGENT_MCP_REQUEST_TIMEOUT_MS=0 OPENAI_API_KEY=test-key \
    "${LAUNCHER}" --env-file "${TMP_ROOT}/empty.env" --dry-run --fs-root "${TMP_ROOT}/fs-root"
expect_failure 'filesystem root is not an existing directory' \
    "${LAUNCHER}" --env-file "${TMP_ROOT}/empty.env" --dry-run --fs-root "${TMP_ROOT}/missing"
expect_failure 'OPENAI_API_KEY or DEEPSEEK_API_KEY is required' \
    "${LAUNCHER}" --env-file "${TMP_ROOT}/empty.env" --dry-run --fs-root "${TMP_ROOT}/fs-root"

printf 'ui-launcher-tests-ok\n'
