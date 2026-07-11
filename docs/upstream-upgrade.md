# Taskflow Upstream Upgrade

## Provenance

- Fork source branch: `dev-agent-1.5`
- Fork source commit: `767be915ba8a1937abbcc28abaa573a984dc510e`
- Fork source date: 2026-04-12
- Declared fork version before upgrade: Taskflow 3.11.0
- Official upstream: `https://github.com/taskflow/taskflow.git`
- Upgrade target branch: `upstream/master`
- Upgrade target commit: `46a9c1ab260950dcc1c50dd9ae8b1398929d370f`
- Upgrade target date: 2026-07-06
- Latest official release observed during the upgrade: `v4.1.0`

## Integration Policy

The official repository remains authoritative for the Taskflow core under
`taskflow/` and its upstream tests, examples, benchmarks, CMake support, and
documentation. The fork remains authoritative for `workflow/`,
`agent_framework/`, fork-specific build options, and the additional git
submodules required by those components. An upstream deletion must therefore
not delete a fork-only component.

The fork continues to default to C++20 because its Workflow and Agent Framework
use C++20 language features even where the upstream Taskflow core supports an
older standard.

## Verification Matrix

| Layer | Configure/build directory | Required result |
| --- | --- | --- |
| Taskflow CPU core | `build-upgrade-core` | Configure, build, and CTest pass |
| Workflow | `build-upgrade-workflow` | Configure, build, and CTest pass |
| Full Agent Framework | `build-upgrade-full` | Configure, build, and offline CTest pass |

CUDA and live network/LLM integration tests are outside the default offline
verification matrix unless the required toolchain and credentials are present.
The final section of this document will record exact test totals and any skips.

## Verification Results

Verified on branch `upgrade/taskflow-4x` at commit `c69777ba8` (merge commit).

### Environment

- OS: Linux (kernel 5.10)
- Compiler: GCC 11.4.0
- CMake: 3.29.0
- C++ standard: 20
- CUDA: disabled for offline matrix

### Configure commands

```bash
cmake -S . -B build-upgrade-core -DCMAKE_BUILD_TYPE=Debug \
  -DTF_BUILD_TESTS=ON -DTF_BUILD_EXAMPLES=OFF \
  -DTF_BUILD_WORKFLOW=OFF -DTF_BUILD_AGENT_FRAMEWORK=OFF -DTF_BUILD_CUDA=OFF

cmake -S . -B build-upgrade-workflow -DCMAKE_BUILD_TYPE=Debug \
  -DTF_BUILD_TESTS=ON -DTF_BUILD_EXAMPLES=OFF \
  -DTF_BUILD_WORKFLOW=ON -DTF_BUILD_AGENT_FRAMEWORK=OFF -DTF_BUILD_CUDA=OFF

cmake -S . -B build-upgrade-full -DCMAKE_BUILD_TYPE=Debug \
  -DTF_BUILD_TESTS=ON -DTF_BUILD_EXAMPLES=OFF \
  -DTF_BUILD_WORKFLOW=ON -DTF_BUILD_AGENT_FRAMEWORK=ON -DTF_BUILD_CUDA=OFF
```

### CTest totals

| Build directory | Tests | Passed | Failed | Skipped | Wall time |
| --- | ---: | ---: | ---: | ---: | --- |
| `build-upgrade-core` | 2919 | 2919 | 0 | 0 | ~260 s |
| `build-upgrade-workflow` | 2919 | 2919 | 0 | 0 | ~265 s |
| `build-upgrade-full` | 2984 | 2984 | 0 | 0 | ~270 s |

### Adaptation notes

- **Taskflow core / Workflow**: no source changes required; existing code compiled against Taskflow 4.1 headers without API edits.
- **Agent Framework**: one test fix in `test_prompt_renderer_wp4.cpp` — `Message` aggregate initialization was updated to match the current six-field layout (`tool_call_id`, `tool_name`, `tool_result`, `timestamp`).
- **Merge conflicts resolved** in `CMakeLists.txt`, `README.md`, `examples/simple.cpp`, and `.github/workflows/ubuntu.yml` (fork Workflow/Agent options and C++20 CI retained alongside upstream modules support).

### Known limitations

- Upstream ancestry check `git merge-base --is-ancestor upstream/master HEAD` may report false on shallow/partial upstream fetches; the merge commit records upstream parent `0ebd18849` (grafted from `46a9c1ab`).
- CUDA builds, live LLM credentials, and GUI demos (`imgui_agent_demo`) were not exercised in this offline pass.
- `AGENT_BUILD_EXAMPLES=OFF` during full configure; CLI/GUI examples were not rebuilt in `build-upgrade-full`.
