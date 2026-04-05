# Tier D agent personas (WP2.a2a-test)

Run two [`agent_server_demo`](../../../examples/agent_server_demo.cpp) processes with different roles and ports.

| 画像 ID | `AGENT_SERVER_DEMO_ROLE` | Card `name` | Skill (wire `name`) | 行为 |
|---------|---------------------------|-------------|----------------------|------|
| **T-D-A** | `integration-worker` | `integration-worker` | `skill.execute` | 完成任务并在 history 中追加含 `SIG_TIER_D_A` 的 agent 文本 |
| **T-D-B** | `integration-reviewer` | `integration-reviewer` | `skill.review` | 若用户消息含 `SIG_TIER_D_A` 则 **COMPLETED**，否则 **FAILED** |

## Example (two terminals)

```bash
# Terminal A
AGENT_SERVER_PORT=9101 AGENT_SERVER_CARD_PUBLIC_BASE=http://127.0.0.1:9101 \
  AGENT_SERVER_DEMO_ROLE=integration-worker ./agent_server_demo

# Terminal B
AGENT_SERVER_PORT=9102 AGENT_SERVER_CARD_PUBLIC_BASE=http://127.0.0.1:9102 \
  AGENT_SERVER_DEMO_ROLE=integration-reviewer ./agent_server_demo
```

Then:

```bash
export AGENT_A2A_MULTI_LIVE=1
export AGENT_A2A_LIVE_AGENT_A_URL=http://127.0.0.1:9101
export AGENT_A2A_LIVE_AGENT_B_URL=http://127.0.0.1:9102
ctest -R a2a_live_multi_agent --verbose
```

Optional: `AGENT_SERVER_AUTH_TOKEN` / `AGENT_A2A_LIVE_TOKEN` for Bearer on both servers and the test client.
