/**
 * @file schema_validate.cpp
 * @brief JSON Schema 子集实现（WP1.2）
 */

#include "agent/schema_validate.hpp"

#include <cmath>
#include <string>
#include <string_view>
#include <unordered_set>

namespace agent_framework {
namespace {

void fill_error(json& error_obj, const std::string& code, const std::string& message, json details) {
    error_obj = json::object();
    error_obj["error"] = message;
    error_obj["code"] = code;
    error_obj["details"] = std::move(details);
}

bool schema_has_unsupported_keywords(const json& schema, std::string& bad_key) {
    static const std::unordered_set<std::string> k_compound = {
        "allOf", "anyOf", "oneOf", "not", "if", "then", "else",
        "dependentSchemas", "dependentRequired", "prefixItems", "contains"};
    static const std::unordered_set<std::string> k_ignored_dollar_metadata = {"$schema", "$id",
                                                                              "$comment"};

    if (!schema.is_object()) {
        return false;
    }
    for (auto it = schema.begin(); it != schema.end(); ++it) {
        const std::string& k = it.key();
        if (!k.empty() && k[0] == '$') {
            if (k_ignored_dollar_metadata.count(k) != 0U) {
                continue;
            }
            bad_key = k;
            return true;
        }
        if (k_compound.count(k) != 0U) {
            bad_key = k;
            return true;
        }
    }
    return false;
}

bool is_json_integer(const json& j) {
    if (j.is_number_integer() || j.is_number_unsigned()) {
        return true;
    }
    if (!j.is_number_float()) {
        return false;
    }
    double d = j.get<double>();
    if (!std::isfinite(d)) {
        return false;
    }
    return d == std::trunc(d);
}

bool value_matches_enum(const json& instance, const json& enum_array) {
    for (const auto& v : enum_array) {
        if (instance == v) {
            return true;
        }
    }
    return false;
}

std::string join_path(const std::string& base, std::string_view key) {
    if (base.empty()) {
        return std::string("/") + std::string(key);
    }
    return base + "/" + std::string(key);
}

bool validate_against_schema(const json& schema, const json& instance, const std::string& path,
                              json& error_obj);

bool validate_object(const json& schema, const json& instance, const std::string& path,
                       json& error_obj) {
    if (!instance.is_object()) {
        fill_error(error_obj, "validation_failed", "expected object",
                   json{{"path", path.empty() ? std::string("/") : path},
                        {"expected", "object"}});
        return false;
    }

    bool allow_additional = false;
    if (schema.contains("additionalProperties")) {
        const auto& ap = schema["additionalProperties"];
        if (!ap.is_boolean()) {
            fill_error(error_obj, "validation_failed", "additionalProperties must be boolean",
                       json{{"path", path.empty() ? std::string("/") : path}});
            return false;
        }
        allow_additional = ap.get<bool>();
    }

    if (schema.contains("required")) {
        const auto& req = schema["required"];
        if (!req.is_array()) {
            fill_error(error_obj, "validation_failed", "required must be array",
                       json{{"path", path}});
            return false;
        }
        json missing = json::array();
        for (const auto& item : req) {
            if (!item.is_string()) {
                fill_error(error_obj, "validation_failed", "required items must be strings",
                           json{{"path", path}});
                return false;
            }
            const std::string& rk = item.get_ref<const std::string&>();
            if (instance.find(rk) == instance.end()) {
                missing.push_back(rk);
            }
        }
        if (!missing.empty()) {
            fill_error(error_obj, "validation_failed", "missing required properties",
                       json{{"path", path.empty() ? std::string("/") : path}, {"missing", missing}});
            return false;
        }
    }

    const json* props_ptr = nullptr;
    if (schema.contains("properties")) {
        const auto& props = schema["properties"];
        if (!props.is_object()) {
            fill_error(error_obj, "validation_failed", "properties must be object",
                       json{{"path", path}});
            return false;
        }
        props_ptr = &props;
    }

    for (auto it = instance.begin(); it != instance.end(); ++it) {
        const std::string& key = it.key();
        if (props_ptr != nullptr) {
            auto pit = props_ptr->find(key);
            if (pit != props_ptr->end()) {
                std::string child_path = join_path(path, key);
                if (!validate_against_schema(*pit, it.value(), child_path, error_obj)) {
                    return false;
                }
                continue;
            }
        }
        if (!allow_additional) {
            fill_error(error_obj, "validation_failed", "additional properties not allowed",
                       json{{"path", join_path(path, key)},
                            {"unexpected_key", key}});
            return false;
        }
    }

    return true;
}

bool validate_array(const json& schema, const json& instance, const std::string& path,
                    json& error_obj) {
    if (!instance.is_array()) {
        fill_error(error_obj, "validation_failed", "expected array",
                   json{{"path", path.empty() ? std::string("/") : path}});
        return false;
    }
    if (schema.contains("items")) {
        const auto& items_schema = schema["items"];
        std::size_t i = 0;
        for (const auto& el : instance) {
            std::string idx = path.empty() ? ("/" + std::to_string(i)) : (path + "/" + std::to_string(i));
            if (!validate_against_schema(items_schema, el, idx, error_obj)) {
                return false;
            }
            ++i;
        }
    }
    return true;
}

bool check_primitive_type(const std::string& type_str, const json& instance, const std::string& path,
                          json& error_obj) {
    if (type_str == "string") {
        if (!instance.is_string()) {
            fill_error(error_obj, "validation_failed", "type mismatch: expected string",
                       json{{"path", path}, {"expected", "string"}});
            return false;
        }
        return true;
    }
    if (type_str == "boolean") {
        if (!instance.is_boolean()) {
            fill_error(error_obj, "validation_failed", "type mismatch: expected boolean",
                       json{{"path", path}, {"expected", "boolean"}});
            return false;
        }
        return true;
    }
    if (type_str == "number") {
        if (!instance.is_number()) {
            fill_error(error_obj, "validation_failed", "type mismatch: expected number",
                       json{{"path", path}, {"expected", "number"}});
            return false;
        }
        return true;
    }
    if (type_str == "integer") {
        if (!is_json_integer(instance)) {
            fill_error(error_obj, "validation_failed", "type mismatch: expected integer",
                       json{{"path", path}, {"expected", "integer"}});
            return false;
        }
        return true;
    }
    fill_error(error_obj, "validation_failed", "unsupported or unknown type in schema",
               json{{"path", path}, {"type", type_str}});
    return false;
}

bool validate_against_schema(const json& schema, const json& instance, const std::string& path,
                              json& error_obj) {
    std::string bad;
    if (schema_has_unsupported_keywords(schema, bad)) {
        fill_error(error_obj, "schema_unsupported", "JSON Schema keyword not supported in WP1.2 subset",
                   json{{"path", path}, {"keyword", bad}});
        return false;
    }

    if (schema.contains("enum")) {
        const auto& en = schema["enum"];
        if (!en.is_array()) {
            fill_error(error_obj, "validation_failed", "enum must be array",
                       json{{"path", path}});
            return false;
        }
        if (!value_matches_enum(instance, en)) {
            fill_error(error_obj, "validation_failed", "value not in enum",
                       json{{"path", path}});
            return false;
        }
        if (!schema.contains("type")) {
            return true;
        }
    }

    std::string type_token;
    if (schema.contains("type")) {
        const auto& t = schema["type"];
        if (!t.is_string()) {
            fill_error(error_obj, "validation_failed", "type must be string",
                       json{{"path", path}});
            return false;
        }
        type_token = t.get<std::string>();
    } else if (schema.contains("properties") || schema.contains("required")) {
        type_token = "object";
    } else if (schema.contains("items")) {
        type_token = "array";
    } else {
        fill_error(error_obj, "validation_failed", "schema must declare type or properties/required/items",
                   json{{"path", path}});
        return false;
    }

    if (type_token == "object") {
        return validate_object(schema, instance, path, error_obj);
    }
    if (type_token == "array") {
        return validate_array(schema, instance, path, error_obj);
    }
    return check_primitive_type(type_token, instance, path, error_obj);
}

} // namespace

void extract_json_schema_root_meta(const json& root_schema, JsonSchemaRootMeta& out) {
    out.json_schema_uri.reset();
    out.id_uri.reset();
    out.comment.reset();
    if (!root_schema.is_object()) {
        return;
    }
    auto assign_opt_string = [](const json& v, std::optional<std::string>& slot) {
        if (v.is_string()) {
            slot = v.get<std::string>();
        } else {
            slot = v.dump();
        }
    };
    if (auto it = root_schema.find("$schema"); it != root_schema.end()) {
        assign_opt_string(*it, out.json_schema_uri);
    }
    if (auto it = root_schema.find("$id"); it != root_schema.end()) {
        assign_opt_string(*it, out.id_uri);
    }
    if (auto it = root_schema.find("$comment"); it != root_schema.end()) {
        assign_opt_string(*it, out.comment);
    }
}

bool validate_tool_arguments(const json& schema, const json& arguments, json& error_obj,
                             JsonSchemaRootMeta* root_meta_out) {
    if (root_meta_out != nullptr) {
        extract_json_schema_root_meta(schema, *root_meta_out);
    }
    if (!schema.is_object()) {
        fill_error(error_obj, "validation_failed", "schema must be a JSON object", json::object());
        return false;
    }
    std::string bad;
    if (schema_has_unsupported_keywords(schema, bad)) {
        fill_error(error_obj, "schema_unsupported", "JSON Schema keyword not supported in WP1.2 subset",
                   json{{"path", "/"}, {"keyword", bad}});
        return false;
    }
    if (!schema.contains("type") || !schema["type"].is_string() ||
        schema["type"].get<std::string>() != "object") {
        fill_error(error_obj, "validation_failed", "root schema type must be object",
                   json{{"path", "/"}});
        return false;
    }
    if (!arguments.is_object()) {
        fill_error(error_obj, "validation_failed", "arguments must be a JSON object",
                   json{{"path", "/"}});
        return false;
    }
    return validate_object(schema, arguments, "", error_obj);
}

} // namespace agent_framework
