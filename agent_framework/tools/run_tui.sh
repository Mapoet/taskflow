#!/usr/bin/env bash
# Configure, build, and launch the full Agent Framework TUI integration.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd -P)"
RAW_ARGS=("$@")

die() {
    printf 'run_tui.sh: %s\n' "$*" >&2
    exit 2
}

usage() {
    cat <<'EOF'
Usage: agent_framework/tools/run_tui.sh [options] [-- TUI_OPTIONS...]

Configure, build, and launch tui_agent_demo with FS, WEB, ExprTk, Draw,
Skills, and (by default) Cursor MCP integration enabled.

Options:
  --build-dir PATH         CMake build directory (default: build-tui)
  --build-type TYPE        Release or Debug (default: Release)
  --fs-root PATH           Filesystem jail root (default: repository root)
  --env-file PATH          Source a trusted local environment file
  --provider NAME          openai or anthropic
  --model NAME             Override AGENT_LLM_MODEL
  -p, --prompt TEXT        Submit an initial prompt after launch
  --max-iterations N       Positive Agent loop iteration limit
  --cursor-mcp-json PATH   Cursor mcp.json path
  --no-cursor-mcp          Disable Cursor MCP import
  --no-build               Run an existing TUI binary without configuring
  --reconfigure            Remove the selected build directory before configure
  --dry-run                Validate and print the redacted launch plan only
  -v, --verbose            Enable Agent debug logging and TUI verbose mode
  -h, --help               Show this help

Environment:
  AGENT_TUI_ENV_FILE, AGENT_TUI_BUILD_DIR, AGENT_TUI_BUILD_TYPE,
  AGENT_TUI_BUILD_JOBS, standard AGENT_*, OPENAI_*, DEEPSEEK_*, and
  ANTHROPIC_* variables are honored. A repository-local .env.tui is loaded
  automatically when present. Environment files are shell syntax and must be
  trusted. API keys are never printed by this launcher.
EOF
}

# Locate the environment file before reading defaults from it.
ENV_FILE="${AGENT_TUI_ENV_FILE:-}"
for ((i = 0; i < ${#RAW_ARGS[@]}; ++i)); do
    case "${RAW_ARGS[$i]}" in
        --env-file)
            ((i + 1 < ${#RAW_ARGS[@]})) || die "--env-file requires a path"
            ENV_FILE="${RAW_ARGS[$((i + 1))]}"
            ;;
        --env-file=*) ENV_FILE="${RAW_ARGS[$i]#*=}" ;;
    esac
done
if [[ -z "${ENV_FILE}" && -f "${REPO_ROOT}/.env.tui" ]]; then
    ENV_FILE="${REPO_ROOT}/.env.tui"
fi
if [[ -n "${ENV_FILE}" ]]; then
    [[ -r "${ENV_FILE}" && -f "${ENV_FILE}" ]] || die "environment file is not readable: ${ENV_FILE}"
    set -a
    # shellcheck disable=SC1090 -- explicitly trusted user-selected configuration.
    source "${ENV_FILE}"
    set +a
fi

BUILD_DIR="${AGENT_TUI_BUILD_DIR:-${REPO_ROOT}/build-tui}"
BUILD_TYPE="${AGENT_TUI_BUILD_TYPE:-Release}"
FS_ROOT="${AGENT_FS_ROOT:-${REPO_ROOT}}"
PROVIDER="${AGENT_LLM_PROVIDER:-openai}"
MODEL="${AGENT_LLM_MODEL:-}"
PROMPT=""
MAX_ITERATIONS=""
MCP_TIMEOUT_MS="${AGENT_MCP_REQUEST_TIMEOUT_MS:-60000}"
CURSOR_MCP_JSON=""
NO_CURSOR_MCP=0
NO_BUILD=0
RECONFIGURE=0
DRY_RUN=0
VERBOSE=0
TUI_EXTRA_ARGS=()

while (($#)); do
    case "$1" in
        --build-dir) (($# >= 2)) || die "--build-dir requires a path"; BUILD_DIR="$2"; shift 2 ;;
        --build-dir=*) BUILD_DIR="${1#*=}"; shift ;;
        --build-type) (($# >= 2)) || die "--build-type requires a value"; BUILD_TYPE="$2"; shift 2 ;;
        --build-type=*) BUILD_TYPE="${1#*=}"; shift ;;
        --fs-root) (($# >= 2)) || die "--fs-root requires a path"; FS_ROOT="$2"; shift 2 ;;
        --fs-root=*) FS_ROOT="${1#*=}"; shift ;;
        --env-file) (($# >= 2)) || die "--env-file requires a path"; shift 2 ;;
        --env-file=*) shift ;;
        --provider) (($# >= 2)) || die "--provider requires a value"; PROVIDER="$2"; shift 2 ;;
        --provider=*) PROVIDER="${1#*=}"; shift ;;
        --model) (($# >= 2)) || die "--model requires a value"; MODEL="$2"; shift 2 ;;
        --model=*) MODEL="${1#*=}"; shift ;;
        -p|--prompt) (($# >= 2)) || die "$1 requires text"; PROMPT="$2"; shift 2 ;;
        --prompt=*) PROMPT="${1#*=}"; shift ;;
        --max-iterations) (($# >= 2)) || die "--max-iterations requires a value"; MAX_ITERATIONS="$2"; shift 2 ;;
        --max-iterations=*) MAX_ITERATIONS="${1#*=}"; shift ;;
        --cursor-mcp-json) (($# >= 2)) || die "--cursor-mcp-json requires a path"; CURSOR_MCP_JSON="$2"; shift 2 ;;
        --cursor-mcp-json=*) CURSOR_MCP_JSON="${1#*=}"; shift ;;
        --no-cursor-mcp) NO_CURSOR_MCP=1; shift ;;
        --no-build) NO_BUILD=1; shift ;;
        --reconfigure) RECONFIGURE=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -v|--verbose) VERBOSE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; TUI_EXTRA_ARGS+=("$@"); break ;;
        *) die "unknown option: $1 (use -- before raw tui_agent_demo options)" ;;
    esac
done

case "${BUILD_TYPE}" in Release|Debug) ;; *) die "--build-type must be Release or Debug" ;; esac
[[ -n "${BUILD_DIR}" ]] || die "build directory must not be empty"
[[ -d "${FS_ROOT}" ]] || die "filesystem root is not an existing directory: ${FS_ROOT}"
FS_ROOT="$(cd -- "${FS_ROOT}" && pwd -P)"
if [[ "${BUILD_DIR}" != /* ]]; then
    BUILD_DIR="${REPO_ROOT}/${BUILD_DIR}"
fi
if [[ -n "${MAX_ITERATIONS}" && ! "${MAX_ITERATIONS}" =~ ^[1-9][0-9]*$ ]]; then
    die "--max-iterations must be a positive integer"
fi
if [[ ! "${MCP_TIMEOUT_MS}" =~ ^[1-9][0-9]*$ ]]; then
    die "AGENT_MCP_REQUEST_TIMEOUT_MS must be a positive integer"
fi
if ((NO_BUILD && RECONFIGURE)); then
    die "--no-build and --reconfigure cannot be used together"
fi
if [[ -n "${CURSOR_MCP_JSON}" && ! -r "${CURSOR_MCP_JSON}" ]]; then
    die "Cursor MCP configuration is not readable: ${CURSOR_MCP_JSON}"
fi

PROVIDER="${PROVIDER,,}"
case "${PROVIDER}" in
    openai)
        if [[ -z "${OPENAI_API_KEY:-}" ]]; then
            [[ -n "${DEEPSEEK_API_KEY:-}" ]] || die "OPENAI_API_KEY or DEEPSEEK_API_KEY is required for provider=openai"
            export OPENAI_API_KEY="${DEEPSEEK_API_KEY}"
            export AGENT_OPENAI_BASE_URL="${AGENT_OPENAI_BASE_URL:-https://api.deepseek.com/v1}"
        else
            export AGENT_OPENAI_BASE_URL="${AGENT_OPENAI_BASE_URL:-https://api.openai.com/v1}"
        fi
        ;;
    anthropic)
        [[ -n "${ANTHROPIC_API_KEY:-}" ]] || die "ANTHROPIC_API_KEY is required for provider=anthropic"
        ;;
    *) die "--provider must be openai or anthropic" ;;
esac

if ((NO_BUILD == 0)); then
    command -v cmake >/dev/null 2>&1 || die "cmake is required"
    command -v c++ >/dev/null 2>&1 || die "a C++ compiler is required"
    if [[ ! "${AGENT_TUI_BUILD_JOBS:-8}" =~ ^[1-9][0-9]*$ ]]; then
        die "AGENT_TUI_BUILD_JOBS must be a positive integer"
    fi
fi

if [[ -z "${LC_ALL:-}" ]] && command -v locale >/dev/null 2>&1 &&
   locale -a 2>/dev/null | grep -qiE '^C\.UTF-?8$'; then
    export LC_ALL=C.UTF-8
fi
export AGENT_LLM_PROVIDER="${PROVIDER}"
[[ -z "${MODEL}" ]] || export AGENT_LLM_MODEL="${MODEL}"
export AGENT_FS_ROOT="${FS_ROOT}"
export AGENT_WEB_ENABLE="${AGENT_WEB_ENABLE:-1}"
export AGENT_EXPR_ENABLE="${AGENT_EXPR_ENABLE:-1}"
export AGENT_DRAW_ENABLE="${AGENT_DRAW_ENABLE:-1}"
export AGENT_SKILL_INJECT_CATALOG="${AGENT_SKILL_INJECT_CATALOG:-1}"
export AGENT_MCP_REQUEST_TIMEOUT_MS="${MCP_TIMEOUT_MS}"
if ((VERBOSE)); then
    export AGENT_LOG_LEVEL=debug
fi

TUI_BINARY="${BUILD_DIR}/agent_framework/tui_agent_demo"
if ((NO_BUILD)) && [[ ! -x "${TUI_BINARY}" ]]; then
    die "TUI binary not found or not executable: ${TUI_BINARY}"
fi
CONFIGURE_CMD=(cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}"
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
    -DTF_BUILD_AGENT_FRAMEWORK=ON
    -DAGENT_BUILD_EXAMPLES=ON
    -DAGENT_BUILD_TUI=ON
    -DBUILD_TESTING=ON)
BUILD_CMD=(cmake --build "${BUILD_DIR}" --target tui_agent_demo --parallel "${AGENT_TUI_BUILD_JOBS:-8}")
RUN_CMD=("${TUI_BINARY}")
[[ -z "${PROMPT}" ]] || RUN_CMD+=(--prompt "${PROMPT}")
[[ -z "${MAX_ITERATIONS}" ]] || RUN_CMD+=(--max-iterations "${MAX_ITERATIONS}")
[[ -z "${CURSOR_MCP_JSON}" ]] || RUN_CMD+=(--cursor-mcp-json "${CURSOR_MCP_JSON}")
((NO_CURSOR_MCP == 0)) || RUN_CMD+=(--no-cursor-mcp)
((VERBOSE == 0)) || RUN_CMD+=(--verbose)
RUN_CMD+=("${TUI_EXTRA_ARGS[@]}")

print_command() {
    printf '  '
    printf '%q ' "$@"
    printf '\n'
}

printf 'Agent Framework TUI launch plan\n'
printf '  repository: %s\n' "${REPO_ROOT}"
printf '  build:      %s (%s)\n' "${BUILD_DIR}" "${BUILD_TYPE}"
printf '  fs jail:    %s\n' "${AGENT_FS_ROOT}"
printf '  provider:   %s\n' "${AGENT_LLM_PROVIDER}"
printf '  model:      %s\n' "${AGENT_LLM_MODEL:-<provider default>}"
printf '  web/expr/draw: %s/%s/%s\n' "${AGENT_WEB_ENABLE}" "${AGENT_EXPR_ENABLE}" "${AGENT_DRAW_ENABLE}"
printf '  skills catalog: %s\n' "${AGENT_SKILL_INJECT_CATALOG}"
printf '  Cursor MCP: %s\n' "$([[ ${NO_CURSOR_MCP} -eq 1 ]] && printf disabled || printf enabled)"
printf '  MCP timeout: %s ms\n' "${AGENT_MCP_REQUEST_TIMEOUT_MS}"
printf '  API credential: configured (redacted)\n'
[[ -z "${ENV_FILE}" ]] || printf '  env file:   %s\n' "${ENV_FILE}"

if ((DRY_RUN)); then
    if ((RECONFIGURE)); then
        print_command cmake -E remove_directory "${BUILD_DIR}"
    fi
    if ((NO_BUILD == 0)); then
        print_command "${CONFIGURE_CMD[@]}"
        print_command "${BUILD_CMD[@]}"
    fi
    print_command "${RUN_CMD[@]}"
    exit 0
fi

[[ -t 0 && -t 1 ]] || die "fullscreen TUI requires an interactive terminal (stdin and stdout must be TTYs)"
[[ "${TERM:-dumb}" != dumb ]] || die "TERM must name a capable terminal, not 'dumb'"

if ((NO_BUILD == 0)); then
    if ((RECONFIGURE)); then
        cmake -E remove_directory "${BUILD_DIR}"
    fi
    "${CONFIGURE_CMD[@]}"
    "${BUILD_CMD[@]}"
fi

[[ -x "${TUI_BINARY}" ]] || die "build completed without producing ${TUI_BINARY}"
exec "${RUN_CMD[@]}"
