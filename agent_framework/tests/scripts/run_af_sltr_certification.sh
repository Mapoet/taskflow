#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:?build directory required}"
build_dir="$(cd "${build_dir}" && pwd -P)"
label="${2:-phase4-offline}"
report_dir="${3:-${build_dir}/agent_framework/certification}"
if [[ "${report_dir}" != /* ]]; then
  report_dir="$(pwd -P)/${report_dir}"
fi
mkdir -p "${report_dir}"

# CTest names are not build-target names. Most executable tests happen to use
# test_<ctest-name>, but script-driven checks (for example
# sqlite_api_boundary) intentionally have no executable target. Query CTest's
# canonical command metadata and build only commands whose executable lives in
# this build tree. The executable basename is the CMake target name generated
# by add_executable().
metadata="$(mktemp)"
trap 'rm -f "${metadata}"' EXIT
ctest --test-dir "${build_dir}" -N -L "${label}" \
  --show-only=json-v1 >"${metadata}"
test_count="$(python3 -c '
import json, sys
with open(sys.argv[1], encoding="utf-8") as stream:
    print(len(json.load(stream).get("tests", [])))
' "${metadata}")"
if [[ "${test_count}" -eq 0 ]]; then
  echo "no tests registered for label: ${label}" >&2
  exit 2
fi
mapfile -t build_targets < <(
  python3 -c '
import json, os, sys
build = os.path.realpath(sys.argv[1]) + os.sep
with open(sys.argv[2], encoding="utf-8") as stream:
    data = json.load(stream)
targets = set()
for test in data.get("tests", []):
    command = test.get("command") or []
    if not command:
        continue
    executable = os.path.realpath(command[0])
    if executable.startswith(build):
        targets.add(os.path.basename(executable))
print("\n".join(sorted(targets)))
' "${build_dir}" "${metadata}"
)

for build_target in "${build_targets[@]}"; do
  cmake --build "${build_dir}" --target "${build_target}" -j2 >/dev/null
done

timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
junit="${report_dir}/af-sltr-${label}-${timestamp}.xml"
ctest --test-dir "${build_dir}" -L "${label}" --output-on-failure \
  --output-junit "${junit}" -j2
printf 'certification_label=%s\ntest_count=%s\njunit=%s\n' \
  "${label}" "${test_count}" "${junit}"
printf 'build_target_count=%s\n' "${#build_targets[@]}"
