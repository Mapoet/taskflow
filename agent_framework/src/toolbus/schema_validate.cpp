/**
 * @file schema_validate.cpp
 * @brief JSON Schema 子集实现（WP1.2）
 */

#include <agent/toolbus/schema_validate.hpp>

#include <cmath>
#include <regex>
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

std::string pointer_token(std::string_view token) {
    std::string out;
    out.reserve(token.size());
    for (char c : token) {
        if (c == '~') out += "~0";
        else if (c == '/') out += "~1";
        else out.push_back(c);
    }
    return out;
}

std::string pointer_child(const std::string& base, std::string_view token) {
    return base + "/" + pointer_token(token);
}

bool generic_error(json& error_obj, const std::string& message,
                   const std::string& instance_path, const std::string& schema_path,
                   const std::string& keyword, json extra = json::object()) {
    extra["instance_path"] = instance_path.empty() ? "/" : instance_path;
    extra["schema_path"] = schema_path.empty() ? "/" : schema_path;
    extra["keyword"] = keyword;
    fill_error(error_obj, "validation_failed", message, std::move(extra));
    return false;
}

bool validate_json_value(const json& schema, const json& instance,
                         const std::string& instance_path, const std::string& schema_path,
                         json& error_obj) {
    if (!schema.is_object()) {
        return generic_error(error_obj, "schema node must be an object", instance_path,
                             schema_path, "schema");
    }
    std::string unsupported;
    if (schema_has_unsupported_keywords(schema, unsupported)) {
        generic_error(error_obj, "JSON Schema keyword is not supported", instance_path,
                      pointer_child(schema_path, unsupported), unsupported);
        error_obj["code"] = "schema_unsupported";
        return false;
    }
    if (schema.contains("enum")) {
        if (!schema["enum"].is_array()) {
            return generic_error(error_obj, "enum must be an array", instance_path,
                                 pointer_child(schema_path, "enum"), "enum");
        }
        if (!value_matches_enum(instance, schema["enum"])) {
            return generic_error(error_obj, "value is not in enum", instance_path,
                                 pointer_child(schema_path, "enum"), "enum");
        }
    }

    std::string type;
    if (schema.contains("type")) {
        if (!schema["type"].is_string()) {
            return generic_error(error_obj, "type must be a string", instance_path,
                                 pointer_child(schema_path, "type"), "type");
        }
        type = schema["type"].get<std::string>();
    } else if (schema.contains("properties") || schema.contains("required")) {
        type = "object";
    } else if (schema.contains("items")) {
        type = "array";
    } else if (schema.contains("enum")) {
        return true;
    } else {
        return generic_error(error_obj, "schema must declare a type", instance_path,
                             schema_path, "type");
    }

    const auto wrong_type = [&](const char* expected) {
        return generic_error(error_obj, std::string("type mismatch: expected ") + expected,
                             instance_path, pointer_child(schema_path, "type"), "type",
                             {{"expected", expected}});
    };
    if (type == "null") return instance.is_null() ? true : wrong_type("null");
    if (type == "boolean") return instance.is_boolean() ? true : wrong_type("boolean");
    if (type == "integer" && !is_json_integer(instance)) return wrong_type("integer");
    if (type == "number" && !instance.is_number()) return wrong_type("number");
    if (type == "string" && !instance.is_string()) return wrong_type("string");
    if (type == "array" && !instance.is_array()) return wrong_type("array");
    if (type == "object" && !instance.is_object()) return wrong_type("object");
    if (type != "null" && type != "boolean" && type != "integer" && type != "number" &&
        type != "string" && type != "array" && type != "object") {
        return generic_error(error_obj, "unsupported schema type", instance_path,
                             pointer_child(schema_path, "type"), "type", {{"type", type}});
    }

    if (type == "number" || type == "integer") {
        const double value = instance.get<double>();
        for (const auto& bound : {std::pair{"minimum", false}, std::pair{"maximum", true}}) {
            if (!schema.contains(bound.first)) continue;
            if (!schema[bound.first].is_number()) {
                return generic_error(error_obj, "numeric bound must be a number", instance_path,
                                     pointer_child(schema_path, bound.first), bound.first);
            }
            const double limit = schema[bound.first].get<double>();
            if ((!bound.second && value < limit) || (bound.second && value > limit)) {
                return generic_error(error_obj, "numeric bound violated", instance_path,
                                     pointer_child(schema_path, bound.first), bound.first,
                                     {{"limit", limit}});
            }
        }
    }
    if (type == "string") {
        const auto& value = instance.get_ref<const std::string&>();
        for (const auto& bound : {std::pair{"minLength", false}, std::pair{"maxLength", true}}) {
            if (!schema.contains(bound.first)) continue;
            if (!schema[bound.first].is_number_unsigned()) {
                return generic_error(error_obj, "string length bound must be unsigned", instance_path,
                                     pointer_child(schema_path, bound.first), bound.first);
            }
            const auto limit = schema[bound.first].get<std::size_t>();
            if ((!bound.second && value.size() < limit) || (bound.second && value.size() > limit)) {
                return generic_error(error_obj, "string length bound violated", instance_path,
                                     pointer_child(schema_path, bound.first), bound.first,
                                     {{"limit", limit}});
            }
        }
        if (schema.contains("pattern")) {
            if (!schema["pattern"].is_string()) {
                return generic_error(error_obj, "pattern must be a string", instance_path,
                                     pointer_child(schema_path, "pattern"), "pattern");
            }
            try {
                if (!std::regex_search(value, std::regex(schema["pattern"].get<std::string>()))) {
                    return generic_error(error_obj, "string does not match pattern", instance_path,
                                         pointer_child(schema_path, "pattern"), "pattern");
                }
            } catch (const std::regex_error&) {
                generic_error(error_obj, "schema pattern is invalid", instance_path,
                              pointer_child(schema_path, "pattern"), "pattern");
                error_obj["code"] = "schema_unsupported";
                return false;
            }
        }
    }
    if (type == "array") {
        for (const auto& bound : {std::pair{"minItems", false}, std::pair{"maxItems", true}}) {
            if (!schema.contains(bound.first)) continue;
            if (!schema[bound.first].is_number_unsigned()) {
                return generic_error(error_obj, "array bound must be unsigned", instance_path,
                                     pointer_child(schema_path, bound.first), bound.first);
            }
            const auto limit = schema[bound.first].get<std::size_t>();
            if ((!bound.second && instance.size() < limit) ||
                (bound.second && instance.size() > limit)) {
                return generic_error(error_obj, "array bound violated", instance_path,
                                     pointer_child(schema_path, bound.first), bound.first,
                                     {{"limit", limit}});
            }
        }
        if (schema.contains("items")) {
            for (std::size_t i = 0; i < instance.size(); ++i) {
                if (!validate_json_value(schema["items"], instance[i],
                                         pointer_child(instance_path, std::to_string(i)),
                                         pointer_child(schema_path, "items"), error_obj)) return false;
            }
        }
    }
    if (type == "object") {
        if (schema.contains("required")) {
            if (!schema["required"].is_array()) {
                return generic_error(error_obj, "required must be an array", instance_path,
                                     pointer_child(schema_path, "required"), "required");
            }
            for (std::size_t i = 0; i < schema["required"].size(); ++i) {
                const auto& required = schema["required"][i];
                if (!required.is_string()) {
                    return generic_error(error_obj, "required item must be a string", instance_path,
                                         pointer_child(pointer_child(schema_path, "required"),
                                                       std::to_string(i)), "required");
                }
                if (!instance.contains(required.get<std::string>())) {
                    return generic_error(error_obj, "missing required property", instance_path,
                                         pointer_child(schema_path, "required"), "required",
                                         {{"missing", required}});
                }
            }
        }
        const json* properties = nullptr;
        if (schema.contains("properties")) {
            if (!schema["properties"].is_object()) {
                return generic_error(error_obj, "properties must be an object", instance_path,
                                     pointer_child(schema_path, "properties"), "properties");
            }
            properties = &schema["properties"];
        }
        bool allow_additional = false;
        if (schema.contains("additionalProperties")) {
            if (!schema["additionalProperties"].is_boolean()) {
                return generic_error(error_obj, "additionalProperties must be boolean", instance_path,
                                     pointer_child(schema_path, "additionalProperties"),
                                     "additionalProperties");
            }
            allow_additional = schema["additionalProperties"].get<bool>();
        }
        for (auto it = instance.begin(); it != instance.end(); ++it) {
            if (properties && properties->contains(it.key())) {
                if (!validate_json_value((*properties)[it.key()], it.value(),
                                         pointer_child(instance_path, it.key()),
                                         pointer_child(pointer_child(schema_path, "properties"), it.key()),
                                         error_obj)) return false;
            } else if (!allow_additional) {
                return generic_error(error_obj, "additional property is not allowed",
                                     pointer_child(instance_path, it.key()),
                                     pointer_child(schema_path, "additionalProperties"),
                                     "additionalProperties", {{"unexpected_key", it.key()}});
            }
        }
    }
    return true;
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

bool validate_json_instance(const json& schema, const json& instance, json& error_obj,
                            JsonSchemaRootMeta* root_meta_out) {
    if (root_meta_out != nullptr) extract_json_schema_root_meta(schema, *root_meta_out);
    return validate_json_value(schema, instance, "", "", error_obj);
}

} // namespace agent_framework
