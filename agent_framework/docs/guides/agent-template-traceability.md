# AgentTemplate 需求—实现—证据追踪

| 阶段 | 主要实现 | 验证证据 |
|---|---|---|
| AT0 | `agent_template/types.hpp`, `types.cpp` | `agent_template_contracts` |
| AT1 | `registry.hpp`, `registry.cpp` | `agent_template_registry` |
| AT2 | `session.hpp`, `session.cpp` | `agent_template_session` |
| AT3 | `planning.hpp`, `planning.cpp` | `agent_template_planning` |
| AT4 | `runner.hpp`, `runner.cpp` | `agent_template_runner_compiler` |
| AT5 | `compiler.hpp`, `compiler.cpp` | DAG parallel/mapping/cancel/missing-runner cases |
| AT6 | `runtime.hpp`, `runtime.cpp` | SQLite E2E、Standalone、WorkflowNode/Subflow、digest drift |
| AT7 | `governance.hpp`, `governance.cpp` | replan CAS/escalation negative、TaskClosure positive |
| AT8 | `operations.hpp`, Operations v1/Web UI | projection round-trip、UI contract、真实浏览器截图 |
| AT9 | 本文、状态和测试矩阵 | `ctest -L agent-template` 与相关 UI/Phase4 回归 |

核心负向保证：future schema、digest tamper、revision conflict、dependency mismatch、permission/budget escalation、cycle、write without approval、required-node removal、committed-effect denial、missing runner、missing completion authority 均 fail closed。
