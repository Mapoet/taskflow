/**
 * @file auth_requirement.cpp
 * @brief Parse securitySchemes → AuthRequirement
 */
#include <agent/a2a/auth_requirement.hpp>

#include <cctype>

namespace agent_framework {
namespace a2a {

namespace {

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool parse_single_scheme(const json& sch, AuthRequirement* part, std::string* warn_out, bool strict) {
    if (!sch.is_object()) {
        if (strict) {
            return false;
        }
        if (warn_out) {
            *warn_out += "auth: skip non-object securityScheme entry\n";
        }
        return false;
    }
    if (!sch.contains("type") || !sch["type"].is_string()) {
        if (strict) {
            return false;
        }
        if (warn_out) {
            *warn_out += "auth: scheme missing type\n";
        }
        return false;
    }
    const std::string type = sch["type"].get<std::string>();
    *part = AuthRequirement{};

    if (ieq(type, "http")) {
        if (!sch.contains("scheme") || !sch["scheme"].is_string()) {
            if (strict) {
                return false;
            }
            if (warn_out) {
                *warn_out += "auth: http scheme missing scheme field\n";
            }
            return false;
        }
        if (!ieq(sch["scheme"].get<std::string>(), "bearer")) {
            if (strict) {
                return false;
            }
            if (warn_out) {
                *warn_out += "auth: only http scheme bearer supported\n";
            }
            return false;
        }
        part->kind = AuthRequirementKind::HttpBearer;
        return true;
    }
    if (ieq(type, "apiKey")) {
        if (!sch.contains("in") || !sch["in"].is_string() || !sch.contains("name") ||
            !sch["name"].is_string()) {
            if (strict) {
                return false;
            }
            if (warn_out) {
                *warn_out += "auth: apiKey missing in/name\n";
            }
            return false;
        }
        const std::string in = sch["in"].get<std::string>();
        const std::string name = sch["name"].get<std::string>();
        if (ieq(in, "header")) {
            part->kind = AuthRequirementKind::ApiKeyHeader;
            part->api_key_header_name = name;
            return true;
        }
        if (ieq(in, "query")) {
            part->kind = AuthRequirementKind::ApiKeyQuery;
            part->api_key_query_param = name;
            return true;
        }
        if (strict) {
            return false;
        }
        if (warn_out) {
            *warn_out += "auth: apiKey.in must be header or query\n";
        }
        return false;
    }
    if (strict) {
        return false;
    }
    if (warn_out) {
        *warn_out += "auth: unknown securityScheme type: " + type + "\n";
    }
    return false;
}

bool compatible_same_kind(const AuthRequirement& a, const AuthRequirement& b) {
    if (a.kind != b.kind) {
        return false;
    }
    if (a.kind == AuthRequirementKind::ApiKeyHeader) {
        return ieq(a.api_key_header_name, b.api_key_header_name);
    }
    if (a.kind == AuthRequirementKind::ApiKeyQuery) {
        return ieq(a.api_key_query_param, b.api_key_query_param);
    }
    return true;
}

} // namespace

bool parse_auth_requirement(const json& security_schemes_wire,
                            bool strict,
                            std::string* warn_out,
                            AuthRequirement* out) {
    if (!out) {
        return false;
    }
    *out = AuthRequirement{};
    out->kind = AuthRequirementKind::None;

    if (!security_schemes_wire.is_object() || security_schemes_wire.empty()) {
        return true;
    }

    bool have = false;
    AuthRequirement acc;

    for (auto it = security_schemes_wire.begin(); it != security_schemes_wire.end(); ++it) {
        AuthRequirement part;
        if (!parse_single_scheme(it.value(), &part, warn_out, strict)) {
            if (strict) {
                return false;
            }
            continue;
        }
        if (!have) {
            acc = part;
            have = true;
            continue;
        }
        if (!compatible_same_kind(acc, part)) {
            if (strict) {
                return false;
            }
            if (warn_out) {
                *warn_out += "auth: multiple distinct security requirements; using None\n";
            }
            out->kind = AuthRequirementKind::None;
            return true;
        }
    }

    if (have) {
        *out = acc;
    }
    return true;
}

} // namespace a2a
} // namespace agent_framework
