#!/usr/bin/env bash
# Configure, build, and start the Live AgentServer demo.
set -Eeuo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd -P)"
die() { printf 'run_agent_server.sh: %s\n' "$*" >&2; exit 2; }
usage() { cat <<'EOF'
Usage: agent_framework/tools/run_agent_server.sh [options] [-- server options]
  --build-dir PATH       CMake build directory (default build-server)
  --build-type TYPE      Release or Debug (default Release)
  --env-file PATH        Trusted shell env file (.env.server, then .env.ui)
  --port N               Server port (default 8080)
  --fs-root PATH         Filesystem jail root (default repository root)
  --no-build             Start an existing binary
  --dry-run              Print redacted launch plan only
  -h, --help             Show help
All remaining options are passed to agent_server_demo (for example --no-skills).
EOF
}
ENV_FILE="${AGENT_SERVER_ENV_FILE:-}"
BUILD_DIR="${AGENT_SERVER_BUILD_DIR:-${REPO_ROOT}/build-server}"
BUILD_TYPE="${AGENT_SERVER_BUILD_TYPE:-Release}"
PORT="${AGENT_SERVER_PORT:-8080}"
FS_ROOT="${AGENT_FS_ROOT:-${REPO_ROOT}}"
NO_BUILD=0; DRY_RUN=0; EXTRA=()
while (($#)); do case "$1" in
  --env-file) (($# >= 2)) || die '--env-file requires a path'; ENV_FILE="$2"; shift 2;;
  --env-file=*) ENV_FILE="${1#*=}"; shift;;
  --build-dir) (($# >= 2)) || die '--build-dir requires a path'; BUILD_DIR="$2"; shift 2;;
  --build-dir=*) BUILD_DIR="${1#*=}"; shift;;
  --build-type) (($# >= 2)) || die '--build-type requires a value'; BUILD_TYPE="$2"; shift 2;;
  --build-type=*) BUILD_TYPE="${1#*=}"; shift;;
  --port) (($# >= 2)) || die '--port requires a value'; PORT="$2"; shift 2;;
  --port=*) PORT="${1#*=}"; shift;;
  --fs-root) (($# >= 2)) || die '--fs-root requires a path'; FS_ROOT="$2"; shift 2;;
  --fs-root=*) FS_ROOT="${1#*=}"; shift;;
  --no-build) NO_BUILD=1; shift;; --dry-run) DRY_RUN=1; shift;;
  -h|--help) usage; exit 0;; --) shift; EXTRA+=("$@"); break;; *) EXTRA+=("$1"); shift;;
esac; done
if [[ -z "$ENV_FILE" && -f "$REPO_ROOT/.env.server" ]]; then ENV_FILE="$REPO_ROOT/.env.server";
elif [[ -z "$ENV_FILE" && -f "$REPO_ROOT/.env.ui" ]]; then ENV_FILE="$REPO_ROOT/.env.ui"; fi
if [[ -n "$ENV_FILE" ]]; then [[ -r "$ENV_FILE" ]] || die "unreadable environment file: $ENV_FILE"; set -a; source "$ENV_FILE"; set +a; fi
[[ "$PORT" =~ ^[1-9][0-9]*$ ]] && ((PORT <= 65535)) || die '--port must be 1..65535'
[[ -d "$FS_ROOT" ]] || die "filesystem root does not exist: $FS_ROOT"
FS_ROOT="$(cd -- "$FS_ROOT" && pwd -P)"
export AGENT_VERIFIER="${AGENT_VERIFIER:-on}" AGENT_WEB_ENABLE="${AGENT_WEB_ENABLE:-1}" AGENT_EXPR_ENABLE="${AGENT_EXPR_ENABLE:-1}" AGENT_DRAW_ENABLE="${AGENT_DRAW_ENABLE:-1}"
if ((DRY_RUN)); then printf 'build_dir=%s\nport=%s\nfs_root=%s\nverifier=%s\nauth=%s\n' "$BUILD_DIR" "$PORT" "$FS_ROOT" "$AGENT_VERIFIER" "${AGENT_SERVER_AUTH_TOKEN:+configured}"; exit 0; fi
if ((NO_BUILD == 0)); then cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DAGENT_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE="$BUILD_TYPE"; cmake --build "$BUILD_DIR" --target agent_server_demo -j"${AGENT_SERVER_BUILD_JOBS:-8}"; fi
exec "$BUILD_DIR/agent_framework/agent_server_demo" --port "$PORT" --fs-root "$FS_ROOT" "${EXTRA[@]}"
