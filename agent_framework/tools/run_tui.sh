#!/usr/bin/env bash
# Backward-compatible TUI entry point. Prefer run_ui.sh --ui tui.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
exec "${SCRIPT_DIR}/run_ui.sh" --ui tui "$@"
