# Public header module migration

Stage 10 aligns the public include tree with `agent_framework/src`. New code should use the
canonical module paths immediately. The former flat paths remain forwarding headers for one
release cycle so existing source and installed consumers continue to compile.

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

```cpp
// Legacy, supported for one compatibility release.
#include <agent/skill_runtime.hpp>
#include <agent/toolbus.hpp>

// Canonical.
#include <agent/skills/skill_runtime.hpp>
#include <agent/toolbus/toolbus.hpp>
```

The compatibility headers contain no declarations of their own. They only forward to the
canonical header, so mixing old and new paths in one translation unit is safe. Internal sources,
tests, tools, and examples use canonical paths and therefore continuously validate the new API.

## Installation contract

The install rule now copies the complete `include/` tree. This corrects the historical omission
of `include/node` and installs both canonical modules and legacy forwarding headers. CI verifies
representative canonical paths, forwarding paths, Node headers, `skillctl`, and schemas.

The forwarding layer may be removed in the next major API version after downstream projects have
migrated. Removal must be announced in release notes and preceded by a repository-wide search for
flat includes.
