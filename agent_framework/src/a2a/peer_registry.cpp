/**
 * @file peer_registry.cpp
 * @brief WP2.agent2agent peer registry
 */
#include <agent/a2a/peer_registry.hpp>
#include <agent/a2a/client_config.hpp>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace agent_framework {
namespace a2a {

void split_json_rpc_url(const std::string& api_endpoint, std::string& out_base, std::string& out_path) {
    const std::size_t scheme = api_endpoint.find("://");
    if (scheme == std::string::npos) {
        throw std::invalid_argument("split_json_rpc_url: api_endpoint missing scheme");
    }
    const std::size_t path_start = api_endpoint.find('/', scheme + 3);
    if (path_start == std::string::npos) {
        out_base = api_endpoint;
        out_path = "/";
        return;
    }
    out_base = api_endpoint.substr(0, path_start);
    out_path = api_endpoint.substr(path_start);
    if (out_path.empty()) {
        out_path = "/";
    }
}

json resolve_peer_auth_config(const json& auth) {
    if (!auth.is_object()) {
        return auth;
    }
    if (auth.contains("token_env") && auth["token_env"].is_string()) {
        const std::string key = auth["token_env"].get<std::string>();
        json out = auth;
        out.erase("token_env");
        const char* v = std::getenv(key.c_str());
        if (v != nullptr && v[0] != '\0') {
            out["token"] = std::string(v);
        }
        return out;
    }
    return auth;
}

std::shared_ptr<AgentClient> make_rpc_agent_client_for_card(const AgentCard& card, const json& auth_config) {
    std::string rpc_base;
    std::string rpc_path;
    split_json_rpc_url(card.api_endpoint, rpc_base, rpc_path);
    AgentClientOptions rpc_opts;
    rpc_opts.use_legacy_rest = false;
    rpc_opts.json_rpc_path = rpc_path;
    auto cli = std::make_shared<AgentClient>(rpc_base, rpc_opts);
    const json resolved = resolve_peer_auth_config(auth_config);
    if (!resolved.empty()) {
        cli->set_authentication(resolved);
    }
    return cli;
}

bool A2aPeerRegistry::is_valid_peer_id(std::string_view id) {
    if (id.empty()) {
        return false;
    }
    for (unsigned char c : id) {
        if (std::isalnum(c) == 0 && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

A2aPeerConfig A2aPeerRegistry::parse_one_peer(const json& o, std::size_t index) {
    if (!o.is_object()) {
        throw std::invalid_argument("A2aPeerRegistry: peers[" + std::to_string(index) + "] must be object");
    }
    A2aPeerConfig cfg;
    if (!o.contains("id") || !o["id"].is_string()) {
        throw std::invalid_argument("A2aPeerRegistry: peers[" + std::to_string(index) + "].id required string");
    }
    cfg.peer_id = o["id"].get<std::string>();
    if (!is_valid_peer_id(cfg.peer_id)) {
        throw std::invalid_argument("A2aPeerRegistry: invalid peer id: " + cfg.peer_id);
    }
    if (!o.contains("origin") || !o["origin"].is_string()) {
        throw std::invalid_argument("A2aPeerRegistry: peers[" + std::to_string(index) + "].origin required string");
    }
    cfg.origin = o["origin"].get<std::string>();
    if (cfg.origin.find("://") == std::string::npos) {
        throw std::invalid_argument("A2aPeerRegistry: peers[" + std::to_string(index) + "].origin must include scheme");
    }
    if (o.contains("well_known_path") && o["well_known_path"].is_string()) {
        cfg.well_known_path = o["well_known_path"].get<std::string>();
        if (cfg.well_known_path.empty() || cfg.well_known_path[0] != '/') {
            throw std::invalid_argument("A2aPeerRegistry: well_known_path must start with /");
        }
    }
    if (o.contains("auth")) {
        cfg.auth = o["auth"];
    }
    if (o.contains("default_timeout_ms")) {
        if (!o["default_timeout_ms"].is_number_integer()) {
            throw std::invalid_argument("A2aPeerRegistry: default_timeout_ms must be integer");
        }
        cfg.default_timeout_ms = o["default_timeout_ms"].get<int>();
        if (cfg.default_timeout_ms <= 0) {
            throw std::invalid_argument("A2aPeerRegistry: default_timeout_ms must be positive");
        }
    }
    return cfg;
}

void A2aPeerRegistry::load_from_json(const json& j) {
    if (!j.is_object() || !j.contains("peers") || !j["peers"].is_array()) {
        throw std::invalid_argument("A2aPeerRegistry: root must be object with \"peers\" array");
    }
    const json& arr = j["peers"];
    std::vector<A2aPeerConfig> next;
    std::unordered_map<std::string, std::size_t> seen;
    next.reserve(arr.size());
    for (std::size_t i = 0; i < arr.size(); ++i) {
        A2aPeerConfig cfg = parse_one_peer(arr[i], i);
        if (seen.count(cfg.peer_id) != 0U) {
            throw std::invalid_argument("A2aPeerRegistry: duplicate peer id: " + cfg.peer_id);
        }
        seen[cfg.peer_id] = i;
        next.push_back(std::move(cfg));
    }
    pending_ = std::move(next);
    resolved_.clear();
    id_index_.clear();
}

void A2aPeerRegistry::load_from_json_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("A2aPeerRegistry: cannot open peers file: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str());
    load_from_json(j);
}

void A2aPeerRegistry::discover_all() {
    if (pending_.empty()) {
        throw std::logic_error("A2aPeerRegistry: load_from_json before discover_all");
    }
    std::vector<ResolvedPeer> out;
    out.reserve(pending_.size());
    id_index_.clear();
    for (std::size_t i = 0; i < pending_.size(); ++i) {
        const A2aPeerConfig& cfg = pending_[i];
        std::string origin = cfg.origin;
        while (!origin.empty() && origin.back() == '/') {
            origin.pop_back();
        }
        AgentClientOptions dopts;
        dopts.use_legacy_rest = false;
        dopts.json_rpc_path = std::string(kA2aJsonRpcDefaultPath);
        AgentClient discover_cli(origin, dopts);
        const json auth_resolved = resolve_peer_auth_config(cfg.auth);
        if (!auth_resolved.empty()) {
            discover_cli.set_authentication(auth_resolved);
        }
        AgentCard card = discover_cli.discover_agent(cfg.well_known_path).get();
        auto rpc = make_rpc_agent_client_for_card(card, cfg.auth);
        ResolvedPeer r;
        r.config = cfg;
        r.card = std::move(card);
        r.rpc_client = std::move(rpc);
        id_index_[r.config.peer_id] = out.size();
        out.push_back(std::move(r));
    }
    resolved_ = std::move(out);
    pending_.clear();
}

std::vector<std::string> A2aPeerRegistry::peer_ids() const {
    std::vector<std::string> ids;
    ids.reserve(resolved_.size());
    for (const auto& r : resolved_) {
        ids.push_back(r.config.peer_id);
    }
    return ids;
}

const AgentCard& A2aPeerRegistry::card(const std::string& peer_id) const {
    auto it = id_index_.find(peer_id);
    if (it == id_index_.end()) {
        throw std::out_of_range("A2aPeerRegistry: unknown peer_id: " + peer_id);
    }
    return resolved_[it->second].card;
}

AgentClient& A2aPeerRegistry::client(const std::string& peer_id) {
    auto it = id_index_.find(peer_id);
    if (it == id_index_.end()) {
        throw std::out_of_range("A2aPeerRegistry: unknown peer_id: " + peer_id);
    }
    return *resolved_[it->second].rpc_client;
}

const AgentClient& A2aPeerRegistry::client(const std::string& peer_id) const {
    auto it = id_index_.find(peer_id);
    if (it == id_index_.end()) {
        throw std::out_of_range("A2aPeerRegistry: unknown peer_id: " + peer_id);
    }
    return *resolved_[it->second].rpc_client;
}

int A2aPeerRegistry::default_timeout_ms_for(const std::string& peer_id) const {
    auto it = id_index_.find(peer_id);
    if (it == id_index_.end()) {
        throw std::out_of_range("A2aPeerRegistry: unknown peer_id: " + peer_id);
    }
    return resolved_[it->second].config.default_timeout_ms;
}

} // namespace a2a
} // namespace agent_framework
