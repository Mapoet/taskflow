#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../../.." && pwd -P)"
LAUNCHER="${REPO_ROOT}/agent_framework/tools/run_tui.sh"
TMP_ROOT="$(mktemp -d)"
trap 'rm -rf -- "${TMP_ROOT}"' EXIT
mkdir -p "${TMP_ROOT}/home" "${TMP_ROOT}/fs-root"
: >"${TMP_ROOT}/empty.env"

fail() {
    printf 'test_run_tui_launcher: %s\n' "$*" >&2
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

bash -n "${LAUNCHER}"
run_clean "${LAUNCHER}" --help | grep -Fq -- '--dry-run' || fail "help omits --dry-run"

SECRET='launcher-secret-must-not-leak'
OUTPUT="$(run_clean \
    OPENAI_API_KEY="${SECRET}" \
    "${LAUNCHER}" \
    --env-file "${TMP_ROOT}/empty.env" \
    --dry-run \
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
grep -Fq -- 'Cursor MCP: disabled' <<<"${OUTPUT}" || fail "MCP override missing"
grep -Fq -- 'API credential: configured (redacted)' <<<"${OUTPUT}" || fail "credential redaction marker missing"
if grep -Fq -- "${SECRET}" <<<"${OUTPUT}"; then
    fail "secret leaked in dry-run output"
fi

expect_failure '--build-type must be Release or Debug' \
    "${LAUNCHER}" --env-file "${TMP_ROOT}/empty.env" --dry-run --build-type RelWithDebInfo --fs-root "${TMP_ROOT}/fs-root"
expect_failure '--max-iterations must be a positive integer' \
    "${LAUNCHER}" --env-file "${TMP_ROOT}/empty.env" --dry-run --max-iterations 0 --fs-root "${TMP_ROOT}/fs-root"
expect_failure 'AGENT_TUI_BUILD_JOBS must be a positive integer' \
    AGENT_TUI_BUILD_JOBS=0 OPENAI_API_KEY=test-key \
    "${LAUNCHER}" --env-file "${TMP_ROOT}/empty.env" --dry-run --fs-root "${TMP_ROOT}/fs-root"
expect_failure 'filesystem root is not an existing directory' \
    "${LAUNCHER}" --env-file "${TMP_ROOT}/empty.env" --dry-run --fs-root "${TMP_ROOT}/missing"
expect_failure 'OPENAI_API_KEY or DEEPSEEK_API_KEY is required' \
    "${LAUNCHER}" --env-file "${TMP_ROOT}/empty.env" --dry-run --fs-root "${TMP_ROOT}/fs-root"

printf 'tui-launcher-tests-ok\n'
