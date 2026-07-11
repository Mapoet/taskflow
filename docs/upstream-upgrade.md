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

Pending execution on `upgrade/taskflow-4x`.
