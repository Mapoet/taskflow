#!/usr/bin/env bash
# Configure, build, and launch one of the Agent Framework rich UIs.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd -P)"
RAW_ARGS=("$@")

die() {
    printf 'run_ui.sh: %s\n' "$*" >&2
    exit 2
}

usage() {
    cat <<'EOF'
Usage: agent_framework/tools/run_ui.sh [options] [-- UI_OPTIONS...]

Configure, build, and launch the TUI, ImGui, or Web UI with FS, WEB,
ExprTk, Draw, Skills, and (by default) Cursor MCP integration enabled.

Options:
  --ui NAME                tui, imgui, or web (default: tui)
  --build-dir PATH         CMake build directory (default: build-ui)
  --build-type TYPE        Release or Debug (default: Release)
  --fs-root PATH           Filesystem jail root (default: agent_framework/tools)
  --skills-root PATH       Installed/read-only Skill root (default: ~/.codex/skills)
  --skill-authoring-root PATH  Writable root for /skills create
  --workbench-state-dir PATH  Durable TUI/ImGui Session and settings directory
  --no-skills              Disable Skill discovery and management
  --env-file PATH          Source a trusted local environment file
  --provider NAME          openai or anthropic
  --model NAME             Override AGENT_LLM_MODEL
  -p, --prompt TEXT        Submit an initial prompt after launch
  --max-iterations N       Positive Agent loop iteration limit
  --port N                 Web listen port (default: 8080; web only)
  --cursor-mcp-json PATH   Cursor mcp.json path
  --no-cursor-mcp          Disable Cursor MCP import
  --skip-mcp-service NAME  Skip one Cursor MCP service (repeatable)
  --demo-state             Load deterministic UI sample data without an LLM call
  --no-build               Run an existing selected binary without configuring
  --reconfigure            Remove the selected build directory before configure
  --dry-run                Validate and print the redacted launch plan only
  -v, --verbose            Enable Agent debug logging and UI verbose mode
  -h, --help               Show this help

Environment:
  AGENT_UI_ENV_FILE, AGENT_UI_BUILD_DIR, AGENT_UI_BUILD_TYPE,
  AGENT_UI_BUILD_JOBS, standard AGENT_*, OPENAI_*, DEEPSEEK_*, and
  ANTHROPIC_* variables are honored. Legacy AGENT_TUI_* build variables and
  .env.tui remain supported. A repository-local .env.ui takes precedence.
  Environment files are trusted shell syntax. API keys are never printed.
EOF
}

# Locate the environment file before reading defaults from it.
ENV_FILE="${AGENT_UI_ENV_FILE:-${AGENT_TUI_ENV_FILE:-}}"
for ((i = 0; i < ${#RAW_ARGS[@]}; ++i)); do
    case "${RAW_ARGS[$i]}" in
        --env-file)
            ((i + 1 < ${#RAW_ARGS[@]})) || die "--env-file requires a path"
            ENV_FILE="${RAW_ARGS[$((i + 1))]}"
            ;;
        --env-file=*) ENV_FILE="${RAW_ARGS[$i]#*=}" ;;
    esac
done
if [[ -z "${ENV_FILE}" && -f "${REPO_ROOT}/.env.ui" ]]; then
    ENV_FILE="${REPO_ROOT}/.env.ui"
elif [[ -z "${ENV_FILE}" && -f "${REPO_ROOT}/.env.tui" ]]; then
    ENV_FILE="${REPO_ROOT}/.env.tui"
fi
if [[ -n "${ENV_FILE}" ]]; then
    [[ -r "${ENV_FILE}" && -f "${ENV_FILE}" ]] || die "environment file is not readable: ${ENV_FILE}"
    set -a
    # shellcheck disable=SC1090 -- explicitly trusted user-selected configuration.
    source "${ENV_FILE}"
    set +a
fi

UI="${AGENT_UI:-tui}"
BUILD_DIR="${AGENT_UI_BUILD_DIR:-${AGENT_TUI_BUILD_DIR:-${REPO_ROOT}/build-ui}}"
BUILD_TYPE="${AGENT_UI_BUILD_TYPE:-${AGENT_TUI_BUILD_TYPE:-Release}}"
BUILD_JOBS="${AGENT_UI_BUILD_JOBS:-${AGENT_TUI_BUILD_JOBS:-8}}"
# Workspace FS jail defaults to this script directory (agent_framework/tools).
FS_ROOT="${AGENT_FS_ROOT:-${SCRIPT_DIR}}"
PROVIDER="${AGENT_LLM_PROVIDER:-openai}"
MODEL="${AGENT_LLM_MODEL:-}"
PROMPT=""
MAX_ITERATIONS=""
PORT="${AGENT_WEB_UI_PORT:-8080}"
MCP_TIMEOUT_MS="${AGENT_MCP_REQUEST_TIMEOUT_MS:-60000}"
CURSOR_MCP_JSON=""
# Prefer an explicit AGENT_SKILLS_DIR; otherwise use the Codex user Skill root.
DEFAULT_SKILLS_ROOT="${HOME}/.codex/skills"
SKILLS_ROOT="${AGENT_SKILLS_DIR:-}"
SKILL_AUTHORING_ROOT="${AGENT_SKILL_AUTHORING_DIR:-}"
WORKBENCH_STATE_DIR="${AGENT_NATIVE_UI_STATE_DIR:-}"
NO_SKILLS=0
NO_CURSOR_MCP=0
DEMO_STATE=0
NO_BUILD=0
RECONFIGURE=0
DRY_RUN=0
VERBOSE=0
UI_EXTRA_ARGS=()
SKIP_MCP_SERVICES=()

while (($#)); do
    case "$1" in
        --ui) (($# >= 2)) || die "--ui requires a value"; UI="$2"; shift 2 ;;
        --ui=*) UI="${1#*=}"; shift ;;
        --build-dir) (($# >= 2)) || die "--build-dir requires a path"; BUILD_DIR="$2"; shift 2 ;;
        --build-dir=*) BUILD_DIR="${1#*=}"; shift ;;
        --build-type) (($# >= 2)) || die "--build-type requires a value"; BUILD_TYPE="$2"; shift 2 ;;
        --build-type=*) BUILD_TYPE="${1#*=}"; shift ;;
        --fs-root) (($# >= 2)) || die "--fs-root requires a path"; FS_ROOT="$2"; shift 2 ;;
        --fs-root=*) FS_ROOT="${1#*=}"; shift ;;
        --skills-root) (($# >= 2)) || die "--skills-root requires a path"; SKILLS_ROOT="$2"; shift 2 ;;
        --skills-root=*) SKILLS_ROOT="${1#*=}"; shift ;;
        --skill-authoring-root) (($# >= 2)) || die "--skill-authoring-root requires a path"; SKILL_AUTHORING_ROOT="$2"; shift 2 ;;
        --skill-authoring-root=*) SKILL_AUTHORING_ROOT="${1#*=}"; shift ;;
        --workbench-state-dir) (($# >= 2)) || die "--workbench-state-dir requires a path"; WORKBENCH_STATE_DIR="$2"; shift 2 ;;
        --workbench-state-dir=*) WORKBENCH_STATE_DIR="${1#*=}"; shift ;;
        --no-skills) NO_SKILLS=1; shift ;;
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
        --port) (($# >= 2)) || die "--port requires a value"; PORT="$2"; shift 2 ;;
        --port=*) PORT="${1#*=}"; shift ;;
        --cursor-mcp-json) (($# >= 2)) || die "--cursor-mcp-json requires a path"; CURSOR_MCP_JSON="$2"; shift 2 ;;
        --cursor-mcp-json=*) CURSOR_MCP_JSON="${1#*=}"; shift ;;
        --no-cursor-mcp) NO_CURSOR_MCP=1; shift ;;
        --skip-mcp-service) (($# >= 2)) || die "--skip-mcp-service requires a name"; SKIP_MCP_SERVICES+=("$2"); shift 2 ;;
        --skip-mcp-service=*) SKIP_MCP_SERVICES+=("${1#*=}"); shift ;;
        --demo-state) DEMO_STATE=1; shift ;;
        --no-build) NO_BUILD=1; shift ;;
        --reconfigure) RECONFIGURE=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -v|--verbose) VERBOSE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; UI_EXTRA_ARGS+=("$@"); break ;;
        *) die "unknown option: $1 (use -- before raw selected-UI options)" ;;
    esac
done

UI="${UI,,}"
case "${UI}" in tui|imgui|web) ;; *) die "--ui must be tui, imgui, or web" ;; esac
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
[[ "${PORT}" =~ ^[1-9][0-9]*$ ]] && ((PORT <= 65535)) || die "--port must be an integer from 1 to 65535"
if [[ "${UI}" != web && "${PORT}" != "8080" ]]; then
    die "--port is only valid with --ui web"
fi
[[ "${MCP_TIMEOUT_MS}" =~ ^[1-9][0-9]*$ ]] || die "AGENT_MCP_REQUEST_TIMEOUT_MS must be a positive integer"
[[ "${BUILD_JOBS}" =~ ^[1-9][0-9]*$ ]] || die "AGENT_UI_BUILD_JOBS must be a positive integer"
if ((NO_BUILD && RECONFIGURE)); then
    die "--no-build and --reconfigure cannot be used together"
fi
if [[ -n "${CURSOR_MCP_JSON}" && ! -r "${CURSOR_MCP_JSON}" ]]; then
    die "Cursor MCP configuration is not readable: ${CURSOR_MCP_JSON}"
fi
# ".codex/skills" is the Codex user Skill root under $HOME.
case "${SKILLS_ROOT}" in
    .codex/skills|./.codex/skills) SKILLS_ROOT="${HOME}/.codex/skills" ;;
esac
if ((NO_SKILLS)); then
    SKILLS_ROOT=""
fi
if [[ -z "${SKILLS_ROOT}" && ${NO_SKILLS} -eq 0 ]]; then
    SKILLS_ROOT="${DEFAULT_SKILLS_ROOT}"
fi
if [[ -n "${SKILLS_ROOT}" ]]; then
    if [[ "${SKILLS_ROOT}" != /* && -d "${REPO_ROOT}/${SKILLS_ROOT}" ]]; then
        SKILLS_ROOT="${REPO_ROOT}/${SKILLS_ROOT}"
    fi
    if [[ ! -d "${SKILLS_ROOT}" ]]; then
        # Create the default Codex Skill root; reject missing explicit overrides.
        if [[ "${SKILLS_ROOT}" == "${DEFAULT_SKILLS_ROOT}" || \
              "${SKILLS_ROOT}" == "$(cd -- "${HOME}" && pwd -P)/.codex/skills" ]]; then
            mkdir -p -- "${SKILLS_ROOT}" || die "cannot create Skill root: ${SKILLS_ROOT}"
        else
            die "Skill root is not an existing directory: ${SKILLS_ROOT}"
        fi
    fi
    SKILLS_ROOT="$(cd -- "${SKILLS_ROOT}" && pwd -P)"
    export AGENT_SKILLS_DIR="${SKILLS_ROOT}"
fi
if [[ -n "${SKILL_AUTHORING_ROOT}" ]]; then
    SKILL_AUTHORING_ROOT="$(realpath -m -- "${SKILL_AUTHORING_ROOT}")"
    mkdir -p -- "${SKILL_AUTHORING_ROOT}" || die "cannot create Skill authoring root: ${SKILL_AUTHORING_ROOT}"
    [[ -w "${SKILL_AUTHORING_ROOT}" ]] || die "Skill authoring root is not writable: ${SKILL_AUTHORING_ROOT}"
    export AGENT_SKILL_AUTHORING_DIR="${SKILL_AUTHORING_ROOT}"
fi
((NO_SKILLS == 0)) || export AGENT_SKILLS_DISABLED=1

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
if ((${#SKIP_MCP_SERVICES[@]})); then
    CLI_SKIP_MCP_SERVICES="$(IFS=,; printf '%s' "${SKIP_MCP_SERVICES[*]}")"
    if [[ -n "${AGENT_MCP_SKIP_SERVICES:-}" ]]; then
        export AGENT_MCP_SKIP_SERVICES="${AGENT_MCP_SKIP_SERVICES},${CLI_SKIP_MCP_SERVICES}"
    else
        export AGENT_MCP_SKIP_SERVICES="${CLI_SKIP_MCP_SERVICES}"
    fi
fi
((VERBOSE == 0)) || export AGENT_LOG_LEVEL=debug

case "${UI}" in
    tui)
        TARGET=tui_agent_demo
        CMAKE_UI_OPTION=-DAGENT_BUILD_TUI=ON
        TUI_BACKEND="FTXUI 7.0.1 (vendored submodule)"
        if ((NO_BUILD == 0)) && [[ ! -f "${REPO_ROOT}/3rd-party/FTXUI/CMakeLists.txt" ]]; then
            die "FTXUI submodule is not initialized; run: git submodule update --init --recursive 3rd-party/FTXUI"
        fi
        ;;
    imgui)
        TARGET=imgui_agent_demo
        CMAKE_UI_OPTION=-DAGENT_BUILD_IMGUI=ON
        ;;
    web)
        TARGET=web_ui_demo
        CMAKE_UI_OPTION=-DAGENT_BUILD_WEB_UI=ON
        if ((NO_BUILD == 0)); then
            command -v npm >/dev/null 2>&1 || die "npm is required to build the formal Web Workbench"
        fi
        ;;
esac
BINARY="${BUILD_DIR}/agent_framework/${TARGET}"
if ((NO_BUILD)) && [[ ! -x "${BINARY}" ]]; then
    die "selected UI binary not found or not executable: ${BINARY}"
fi
CONFIGURE_CMD=(cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}"
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
    -DTF_BUILD_AGENT_FRAMEWORK=ON
    -DAGENT_BUILD_EXAMPLES=ON
    "${CMAKE_UI_OPTION}"
    -DBUILD_TESTING=ON)
BUILD_CMD=(cmake --build "${BUILD_DIR}" --target "${TARGET}" --parallel "${BUILD_JOBS}")
WEB_BUILD_CMD=()
if [[ "${UI}" == web ]]; then
    WEB_BUILD_CMD=(npm --prefix "${REPO_ROOT}/agent_framework/web" run build)
fi
RUN_CMD=("${BINARY}")
[[ "${UI}" != web ]] || RUN_CMD+=(--port "${PORT}")
[[ -z "${PROMPT}" ]] || RUN_CMD+=(--prompt "${PROMPT}")
[[ -z "${MAX_ITERATIONS}" ]] || RUN_CMD+=(--max-iterations "${MAX_ITERATIONS}")
[[ -z "${CURSOR_MCP_JSON}" ]] || RUN_CMD+=(--cursor-mcp-json "${CURSOR_MCP_JSON}")
[[ -z "${SKILLS_ROOT}" || ${NO_SKILLS} -ne 0 ]] || RUN_CMD+=(--skills-root "${SKILLS_ROOT}")
[[ -z "${SKILL_AUTHORING_ROOT}" ]] || RUN_CMD+=(--skill-authoring-root "${SKILL_AUTHORING_ROOT}")
if [[ -n "${WORKBENCH_STATE_DIR}" ]]; then
    [[ "${UI}" != web ]] || die "--workbench-state-dir is only valid with --ui tui or imgui"
    RUN_CMD+=(--workbench-state-dir "${WORKBENCH_STATE_DIR}")
fi
((NO_SKILLS == 0)) || RUN_CMD+=(--no-skills)
((NO_CURSOR_MCP == 0)) || RUN_CMD+=(--no-cursor-mcp)
((DEMO_STATE == 0)) || RUN_CMD+=(--demo-state)
((VERBOSE == 0)) || RUN_CMD+=(--verbose)
RUN_CMD+=("${UI_EXTRA_ARGS[@]}")

print_command() {
    printf '  '
    printf '%q ' "$@"
    printf '\n'
}

printf 'Agent Framework UI launch plan\n'
printf '  interface:  %s (%s)\n' "${UI}" "${TARGET}"
[[ "${UI}" != tui ]] || printf '  TUI backend: %s\n' "${TUI_BACKEND}"
printf '  repository: %s\n' "${REPO_ROOT}"
printf '  build:      %s (%s)\n' "${BUILD_DIR}" "${BUILD_TYPE}"
printf '  fs jail:    %s\n' "${AGENT_FS_ROOT}"
printf '  provider:   %s\n' "${AGENT_LLM_PROVIDER}"
printf '  model:      %s\n' "${AGENT_LLM_MODEL:-<provider default>}"
printf '  web/expr/draw: %s/%s/%s\n' "${AGENT_WEB_ENABLE}" "${AGENT_EXPR_ENABLE}" "${AGENT_DRAW_ENABLE}"
printf '  skills catalog: %s\n' "${AGENT_SKILL_INJECT_CATALOG}"
printf '  skills root: %s\n' "$([[ ${NO_SKILLS} -eq 1 ]] && printf disabled || printf '%s' "${AGENT_SKILLS_DIR:-${DEFAULT_SKILLS_ROOT}}")"
printf '  skill authoring: %s\n' "${AGENT_SKILL_AUTHORING_DIR:-disabled}"
[[ "${UI}" == web ]] || printf '  workbench state: %s\n' "${WORKBENCH_STATE_DIR:-.agent-framework/native-workbench}"
printf '  Cursor MCP: %s\n' "$([[ ${NO_CURSOR_MCP} -eq 1 ]] && printf disabled || printf enabled)"
printf '  demo state: %s\n' "$([[ ${DEMO_STATE} -eq 1 ]] && printf enabled || printf disabled)"
printf '  MCP timeout: %s ms\n' "${AGENT_MCP_REQUEST_TIMEOUT_MS}"
printf '  skipped MCP: %s\n' "${AGENT_MCP_SKIP_SERVICES:-<none>}"
[[ "${UI}" != web ]] || printf '  web URL:     http://127.0.0.1:%s/\n' "${PORT}"
printf '  API credential: configured (redacted)\n'
[[ -z "${ENV_FILE}" ]] || printf '  env file:   %s\n' "${ENV_FILE}"

if ((DRY_RUN)); then
    ((RECONFIGURE == 0)) || print_command cmake -E remove_directory "${BUILD_DIR}"
    if ((NO_BUILD == 0)); then
        ((${#WEB_BUILD_CMD[@]} == 0)) || print_command "${WEB_BUILD_CMD[@]}"
        print_command "${CONFIGURE_CMD[@]}"
        print_command "${BUILD_CMD[@]}"
    fi
    print_command "${RUN_CMD[@]}"
    exit 0
fi

if [[ "${UI}" == tui ]]; then
    [[ -t 0 && -t 1 ]] || die "fullscreen TUI requires an interactive terminal (stdin and stdout must be TTYs)"
    [[ "${TERM:-dumb}" != dumb ]] || die "TERM must name a capable terminal, not 'dumb'"
fi

if ((NO_BUILD == 0)); then
    ((RECONFIGURE == 0)) || cmake -E remove_directory "${BUILD_DIR}"
    ((${#WEB_BUILD_CMD[@]} == 0)) || "${WEB_BUILD_CMD[@]}"
    "${CONFIGURE_CMD[@]}"
    "${BUILD_CMD[@]}"
fi

[[ -x "${BINARY}" ]] || die "build completed without producing ${BINARY}"
exec "${RUN_CMD[@]}"
