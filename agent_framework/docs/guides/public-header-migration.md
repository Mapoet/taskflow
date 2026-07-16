# Public header module migration

Stage 10 aligns the public include tree with `agent_framework/src`. The Stage 10 canonical-only
follow-up removed every flat forwarding header, so module-qualified paths are now the only public
API. This is an intentional source-breaking change for consumers that still include
`agent/<header>.hpp` directly.

## Canonical modules

| Implementation directory | Canonical public prefix | Example |
|---|---|---|
| `src/agent` | `agent/agent/` | `<agent/agent/execution_context.hpp>` |
| `src/agent_client` | `agent/agent_client/` | `<agent/agent_client/agent_client.hpp>` |
| `src/agent_server` | `agent/agent_server/` | `<agent/agent_server/agent_server.hpp>` |
| `src/agent_transport` | `agent/agent_transport/` | `<agent/agent_transport/sse_connection.hpp>` |
| `src/context_budget` | `agent/context_budget/` | `<agent/context_budget/context_budget.hpp>` |
| `src/encoder` | `agent/encoder/` | `<agent/encoder/encoder.hpp>` |
| `src/graph_executor` | `agent/graph_executor/` | `<agent/graph_executor/graph_executor.hpp>` |
| `src/llm_client` | `agent/llm_client/` | `<agent/llm_client/llm_client.hpp>` |
| `src/mcp_client` | `agent/mcp_client/` | `<agent/mcp_client/mcp_client.hpp>` |
| `src/memory` | `agent/memory/` | `<agent/memory/memory.hpp>` |
| `src/prompt_renderer` | `agent/prompt_renderer/` | `<agent/prompt_renderer/prompt_renderer.hpp>` |
| `src/session` | `agent/session/` | `<agent/session/session_store.hpp>` |
| `src/skills` | `agent/skills/` | `<agent/skills/skill_runtime.hpp>` |
| `src/toolbus` | `agent/toolbus/` | `<agent/toolbus/toolbus.hpp>` |
| `src/ui` | `agent/ui/` | `<agent/ui/ui_manager.hpp>` |
| `src/vectorstore` | `agent/vectorstore/` | `<agent/vectorstore/vectorstore.hpp>` |
| cross-module types | `agent/core/` | `<agent/core/types.hpp>` |
| `src/node` | `node/` | `<node/nodes.hpp>` |
| `src/a2a` | `agent/a2a/` | `<agent/a2a/orchestration.hpp>` |

## Source migration

Replace removed flat imports with their module-qualified equivalents:

```cpp
#include <agent/skills/skill_runtime.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/core/types.hpp>
#include <agent/agent/execution_context.hpp>
```

There is no generated alias layer and no opt-in compatibility switch. Downstream projects must
migrate their includes before updating. Internal sources, tests, tools, and examples all use the
same canonical paths as installed consumers.

## Installation contract

The install rule now copies the complete `include/` tree. This corrects the historical omission
of `include/node` and installs canonical Agent modules plus public Node headers. CI verifies
representative canonical paths, Node headers, `skillctl`, and schemas, and explicitly asserts that
representative flat paths are absent.

CMake configuration rejects any new file placed directly under `include/agent/`; public headers
must be owned by a module directory.
