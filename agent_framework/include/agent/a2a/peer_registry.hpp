/**
 * @file peer_registry.hpp
 * @brief WP2.agent2agent: peer config JSON, Card discovery, RPC-scoped AgentClient (orchestration)
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_A2A_PEER_REGISTRY_H__
#define __AGENT_A2A_PEER_REGISTRY_H__

#include <agent/agent_client.hpp>
#include <agent/types.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace agent_framework {
namespace a2a {

/**
 * @brief Split Agent Card `api_endpoint` into HTTP origin (scheme+host+port) and JSON-RPC path.
 */
void split_json_rpc_url(const std::string& api_endpoint, std::string& out_base, std::string& out_path);

/**
 * @brief Resolve `token_env` in auth JSON to `token` using std::getenv (WP2.5 / live tests).
 */
json resolve_peer_auth_config(const json& auth);

/**
 * @brief Build an AgentClient for JSON-RPC POST to the path declared on the Card.
 */
std::shared_ptr<AgentClient> make_rpc_agent_client_for_card(const AgentCard& card, const json& auth_config = {});

/**
 * @brief One row in peers.json (WP2.agent2agent).
 */
struct A2aPeerConfig {
    std::string peer_id;
    std::string origin;
    std::string well_known_path = "/.well-known/agent-card.json";
    json auth = json::object();
    int default_timeout_ms = 120000;
};

/**
 * @brief Load peer table, then discover Well-Known Card per peer and construct RPC clients.
 */
class A2aPeerRegistry {
public:
    /** @brief Parse `peers` array only (no network). */
    void load_from_json(const json& j);

    void load_from_json_file(const std::string& path);

    /** @brief Discover all peers loaded by load_from_json*. */
    void discover_all();

    bool empty() const { return resolved_.empty(); }

    std::size_t size() const { return resolved_.size(); }

    /** @brief Stable peer ids in load order. */
    std::vector<std::string> peer_ids() const;

    const AgentCard& card(const std::string& peer_id) const;

    AgentClient& client(const std::string& peer_id);

    const AgentClient& client(const std::string& peer_id) const;

    /** @brief `default_timeout_ms` from peers.json for this peer. */
    int default_timeout_ms_for(const std::string& peer_id) const;

private:
    struct ResolvedPeer {
        A2aPeerConfig config;
        AgentCard card;
        std::shared_ptr<AgentClient> rpc_client;
    };

    static A2aPeerConfig parse_one_peer(const json& o, std::size_t index);
    static bool is_valid_peer_id(std::string_view id);

    std::vector<A2aPeerConfig> pending_;
    std::vector<ResolvedPeer> resolved_;
    std::unordered_map<std::string, std::size_t> id_index_;
};

} // namespace a2a
} // namespace agent_framework

#endif // __AGENT_A2A_PEER_REGISTRY_H__
