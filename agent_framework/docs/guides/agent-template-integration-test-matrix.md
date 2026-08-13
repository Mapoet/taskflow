# AgentTemplate 集成测试矩阵

| 层级 | 覆盖 | 自动化入口 |
|---|---|---|
| Contract | round-trip、digest、schema、4×5 mode orthogonality | `agent_template_contracts` |
| Store | immutability、CAS、restart、tamper | `agent_template_registry` |
| Resolution | deterministic retrieval、permission intersection、generation isolation | `agent_template_session` |
| Planning | providers、DAG、policy、replan invariant | `agent_template_planning`, `agent_template_governance` |
| Execution | 8 runner kinds、parallel DAG、mapping、cancel | `agent_template_runner_compiler` |
| Integration | Registry→Closure、Workflow node/subflow | `agent_template_runtime` |
| Operations | canonical round-trip、CLI text、Web DOM | `agent_template_operations`, `phase4_operations_ui`, static UI script |
| Visual | actual server/browser at 1440×1000 | `/tmp/af-at8-operations.png` |

生产环境待执行：真实 LLM structured planning；MCP disconnect/reconcile；ChildAgent/A2A retry；process crash checkpoint/attach；approval resume；24h soak；多租户 authorization negative；OTel/SLO correlation。
