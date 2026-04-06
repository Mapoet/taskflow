/**
 * @file orchestration.hpp
 * @brief WP2.agent2agent: session book, remote task wait, ToolBus registration
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_A2A_ORCHESTRATION_H__
#define __AGENT_A2A_ORCHESTRATION_H__

#include <agent/a2a/peer_registry.hpp>
#include <agent/types.hpp>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace agent_framework {

class ToolBus;

namespace a2a {

class OutboundTaskSupervisor;

/** @brief Registered tool name for LLM → remote SendMessage. */
inline constexpr const char* kA2aOrchestratorToolSendMessage = "a2a_send_message";

/**
 * @brief Per-peer A2A context id (maps to AgentClient send_task session_id / wire context).
 */
class PeerSessionBook {
public:
    std::optional<std::string> get(const std::string& peer_id) const;

    void set(const std::string& peer_id, const std::string& session_id);

    void clear(const std::string& peer_id);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> by_peer_;
};

struct A2aRemoteTaskOptions {
    int timeout_ms = 120000;
    bool use_sse_if_capable = true;
    /** Optional one-line log per remote status frame (e.g. stderr). */
    std::function<void(std::string_view peer_id, std::string_view line)> on_remote_log;
};

struct A2aToolRegistrationOptions {
    bool register_per_peer_aliases = false;
    /** If > 0, overrides each peer's `default_timeout_ms` from peers.json for wait. */
    int default_timeout_ms = 0;
    std::function<void(std::string_view peer_id, std::string_view line)> on_remote_log;
};

/**
 * @brief Run SendMessage then wait for terminal state (SSE if streaming capability, else GetTask poll).
 * @return JSON for LLM: ok, peer_id, task_id, status, summary_text, context_id, error, a2a_error_code
 */
json run_remote_task_and_wait(
    const std::string& peer_id,
    AgentClient& rpc_client,
    const AgentCard& card,
    const AgentMessage& message,
    PeerSessionBook* session_book,
    bool continue_session,
    const json& metadata,
    const A2aRemoteTaskOptions& opts);

bool agent_card_has_streaming(const AgentCard& card);

void register_a2a_orchestrator_tools(
    ToolBus& bus,
    A2aPeerRegistry& registry,
    PeerSessionBook& session_book,
    const A2aToolRegistrationOptions& opts = {});

/**
 * @brief Register send_message + WP2.agents fine-grained tools (requires non-null supervisor).
 */
void register_a2a_orchestrator_tools(
    ToolBus& bus,
    A2aPeerRegistry& registry,
    PeerSessionBook& session_book,
    const std::shared_ptr<OutboundTaskSupervisor>& supervisor,
    const A2aToolRegistrationOptions& opts = {});

} // namespace a2a
} // namespace agent_framework

#endif // __AGENT_A2A_ORCHESTRATION_H__
