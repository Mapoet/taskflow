/**
 * @file wire_card.cpp
 * @brief Agent Card wire 映射（WP2.1a）
 */

#include <agent/a2a/wire_card.hpp>

#include <stdexcept>

namespace agent_framework {
namespace a2a {
namespace {

constexpr const char* kDefaultWireAgentVersion = "1.0.0";

void require_string_field(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_string()) {
        throw std::invalid_argument(std::string("agent_card_from_a2a_wire: missing or invalid string field: ") +
                                    key);
    }
}

void require_object_field(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_object()) {
        throw std::invalid_argument(std::string("agent_card_from_a2a_wire: missing or invalid object field: ") +
                                    key);
    }
}

void require_array_field(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_array()) {
        throw std::invalid_argument(std::string("agent_card_from_a2a_wire: missing or invalid array field: ") +
                                    key);
    }
}

json capabilities_to_object(const std::vector<std::string>& caps) {
    json c = json::object();
    c["streaming"] = false;
    c["pushNotifications"] = false;
    c["stateTransitionHistory"] = false;
    for (const auto& s : caps) {
        if (s == "streaming") {
            c["streaming"] = true;
        } else if (s == "push-notifications" || s == "pushNotifications") {
            c["pushNotifications"] = true;
        } else if (s == "state-transition-history" || s == "stateTransitionHistory") {
            c["stateTransitionHistory"] = true;
        }
    }
    return c;
}

void capabilities_from_object(const json& c, std::vector<std::string>& out) {
    out.clear();
    if (!c.is_object()) {
        return;
    }
    if (c.value("streaming", false)) {
        out.push_back("streaming");
    }
    if (c.value("pushNotifications", false)) {
        out.push_back("push-notifications");
    }
    if (c.value("stateTransitionHistory", false)) {
        out.push_back("state-transition-history");
    }
}

json skill_to_wire(const AgentSkill& sk) {
    json sj = json::object();
    sj["id"] = sk.name;
    sj["name"] = sk.name;
    sj["description"] = sk.description;
    json tags = json::array();
    for (const auto& t : sk.required_capabilities) {
        tags.push_back(t);
    }
    sj["tags"] = std::move(tags);
    const bool use_json = (sk.input_schema.is_object() && !sk.input_schema.empty()) ||
                          (sk.output_schema.is_object() && !sk.output_schema.empty());
    if (use_json) {
        sj["inputModes"] = json::array({"application/json"});
        sj["outputModes"] = json::array({"application/json"});
    } else {
        sj["inputModes"] = json::array({"text/plain"});
        sj["outputModes"] = json::array({"text/plain"});
    }
    return sj;
}

AgentSkill skill_from_wire(const json& sj) {
    if (!sj.is_object()) {
        throw std::invalid_argument("agent_card_from_a2a_wire: skill entry must be object");
    }
    require_string_field(sj, "name");
    require_string_field(sj, "description");
    require_array_field(sj, "tags");
    AgentSkill sk;
    sk.name = sj["name"].get<std::string>();
    sk.description = sj["description"].get<std::string>();
    for (const auto& t : sj["tags"]) {
        if (!t.is_string()) {
            throw std::invalid_argument("agent_card_from_a2a_wire: skill.tags must be strings");
        }
        sk.required_capabilities.push_back(t.get<std::string>());
    }
    sk.input_schema = json::object();
    sk.output_schema = json::object();
    return sk;
}

} // namespace

json agent_card_to_a2a_wire(const AgentCard& card) {
    json j = json::object();
    j["name"] = card.name;
    j["description"] = card.description;
    j["protocolVersion"] = "1.0";
    j["supportedInterfaces"] = json::array({{
        {"url", card.api_endpoint},
        {"protocolBinding", "JSONRPC"},
        {"protocolVersion", "1.0"}
    }});
    j["version"] = kDefaultWireAgentVersion;
    j["capabilities"] = capabilities_to_object(card.capabilities);
    j["defaultInputModes"] = json::array({"text/plain"});
    j["defaultOutputModes"] = json::array({"text/plain"});
    if (!card.provider.empty()) {
        j["provider"] = json::object({{"organization", card.provider}, {"url", ""}});
    }
    if (card.authentication_scheme.is_object()) {
        j["securitySchemes"] = card.authentication_scheme;
    } else {
        j["securitySchemes"] = json::object();
    }
    json skills = json::array();
    for (const auto& sk : card.skills) {
        skills.push_back(skill_to_wire(sk));
    }
    j["skills"] = std::move(skills);
    return j;
}

AgentCard agent_card_from_a2a_wire(const json& j) {
    if (!j.is_object()) {
        throw std::invalid_argument("agent_card_from_a2a_wire: root must be object");
    }
    require_string_field(j, "name");
    require_string_field(j, "description");
    const bool has_legacy_url = j.contains("url") && j["url"].is_string();
    const bool has_interfaces = j.contains("supportedInterfaces") &&
                                j["supportedInterfaces"].is_array() &&
                                !j["supportedInterfaces"].empty();
    if (!has_legacy_url && !has_interfaces) {
        throw std::invalid_argument(
            "agent_card_from_a2a_wire: missing supportedInterfaces (or legacy url)");
    }
    require_string_field(j, "version");
    require_object_field(j, "capabilities");
    require_array_field(j, "defaultInputModes");
    require_array_field(j, "defaultOutputModes");
    require_array_field(j, "skills");

    for (const auto& m : j["defaultInputModes"]) {
        if (!m.is_string()) {
            throw std::invalid_argument("agent_card_from_a2a_wire: defaultInputModes must be strings");
        }
    }
    for (const auto& m : j["defaultOutputModes"]) {
        if (!m.is_string()) {
            throw std::invalid_argument("agent_card_from_a2a_wire: defaultOutputModes must be strings");
        }
    }

    AgentCard card;
    card.name = j["name"].get<std::string>();
    card.description = j["description"].get<std::string>();
    if (has_interfaces) {
        const auto& iface = j["supportedInterfaces"].front();
        require_string_field(iface, "url");
        card.api_endpoint = iface["url"].get<std::string>();
    } else {
        card.api_endpoint = j["url"].get<std::string>();
    }
    capabilities_from_object(j["capabilities"], card.capabilities);
    if (j.contains("provider") && j["provider"].is_object()) {
        const auto& p = j["provider"];
        if (p.contains("organization") && p["organization"].is_string()) {
            card.provider = p["organization"].get<std::string>();
        }
    }
    if (j.contains("securitySchemes") && j["securitySchemes"].is_object()) {
        card.authentication_scheme = j["securitySchemes"];
    } else {
        card.authentication_scheme = json::object();
    }
    card.skills.clear();
    for (const auto& sj : j["skills"]) {
        card.skills.push_back(skill_from_wire(sj));
    }
    return card;
}

std::string agent_card_discovery_json_string(const AgentCard& card) {
    return agent_card_to_a2a_wire(card).dump();
}

} // namespace a2a
} // namespace agent_framework
