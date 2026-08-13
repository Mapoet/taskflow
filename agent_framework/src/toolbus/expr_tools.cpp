/**
 * @file expr_tools.cpp
 * @brief ExprTk 内建 expr_eval / expr_validate / expr_batch_eval（AGENT_EXPR_ENABLE）
 */

#include <agent/toolbus/expr_tools.hpp>
#include <agent/context_budget/context_budget.hpp>
#include <agent/core/types.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(AGENT_HAVE_EXPRTK) && AGENT_HAVE_EXPRTK
#include "exprtk.hpp"
#endif

namespace agent_framework {
namespace {

#if defined(AGENT_HAVE_EXPRTK) && AGENT_HAVE_EXPRTK

bool env_truthy_on(const char* v) {
    if (!v || !*v) {
        return false;
    }
    std::string s(v);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

bool env_falsy_off(const char* v) {
    if (!v || !*v) {
        return false;
    }
    std::string s(v);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s == "0" || s == "false" || s == "no" || s == "off";
}

bool expr_register_enabled() {
    const char* v = std::getenv("AGENT_EXPR_ENABLE");
    if (!v || !*v) {
        return true;
    }
    return !env_falsy_off(v);
}

std::size_t env_size_t(const char* name, std::size_t default_v) {
    const char* v = std::getenv(name);
    if (!v || !*v) {
        return default_v;
    }
    char* end = nullptr;
    unsigned long long n = std::strtoull(v, &end, 10);
    if (end == v || *end != '\0' || n == 0ULL) {
        return default_v;
    }
    return static_cast<std::size_t>(std::min(n, static_cast<unsigned long long>(SIZE_MAX)));
}

struct ExprConfig {
    std::size_t max_expr_bytes = 16384;
    std::size_t max_loop_iters = 100000;
    std::size_t parser_stack_depth = 200;
    std::size_t parser_node_depth = 2000;
    bool disable_control_flow = false;
};

ExprConfig load_expr_config_from_env() {
    ExprConfig c;
    c.max_expr_bytes = env_size_t("AGENT_EXPR_MAX_EXPR_BYTES", 16384);
    c.max_loop_iters = env_size_t("AGENT_EXPR_MAX_LOOP_ITERS", 100000);
    c.parser_stack_depth = env_size_t("AGENT_EXPR_PARSER_STACK_DEPTH", 200);
    c.parser_node_depth = env_size_t("AGENT_EXPR_PARSER_NODE_DEPTH", 2000);
    c.disable_control_flow = env_truthy_on(std::getenv("AGENT_EXPR_DISABLE_CONTROL_FLOW"));
    return c;
}

json make_expr_error(const std::string& code, const std::string& message) {
    return json{{"error", json{{"code", code}, {"message", message}}}};
}

std::string truncate_msg(std::string s, std::size_t max_n) {
    return utf8_safe_truncate(s, max_n);
}

using parser_t = exprtk::parser<double>;
using symbol_table_t = exprtk::symbol_table<double>;
using expression_t = exprtk::expression<double>;
using settings_t = parser_t::settings_t;
using symbol_t = parser_t::dependent_entity_collector::symbol_t;
using sym_type_enum = parser_t::symbol_type;

std::string symbol_kind_str(sym_type_enum st) {
    using stype = parser_t::symbol_type;
    switch (st) {
    case stype::e_st_variable:
        return "variable";
    case stype::e_st_vector:
        return "vector";
    case stype::e_st_vecelem:
        return "vector_element";
    case stype::e_st_string:
        return "string";
    case stype::e_st_function:
        return "function";
    case stype::e_st_local_variable:
        return "local_variable";
    case stype::e_st_local_vector:
        return "local_vector";
    case stype::e_st_local_string:
        return "local_string";
    default:
        return "unknown";
    }
}

json parse_error_json(const parser_t& parser) {
    std::string msg = truncate_msg(parser.error(), 512);
    std::string code = "parse_error";
    if (msg.find("ERR184") != std::string::npos) {
        code = "undefined_symbol";
    }
    return make_expr_error(code, msg);
}

struct ExprLoopRtc final : exprtk::loop_runtime_check {
    ExprLoopRtc() {
        loop_set = e_all_loops;
        max_loop_iterations = 100000;
    }
    void set_max(std::uint64_t n) {
        max_loop_iterations = n;
    }
    void handle_runtime_violation(const violation_context&) override {
        throw std::runtime_error("exprtk_loop_limit");
    }
};

void merge_number_object(const json& o, std::map<std::string, double>& out) {
    if (!o.is_object()) {
        return;
    }
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (it.value().is_number()) {
            out[it.key()] = it.value().get<double>();
        }
    }
}

void merge_vectors_object(const json& o, std::map<std::string, std::vector<double>>& out) {
    if (!o.is_object()) {
        return;
    }
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (!it.value().is_array()) {
            continue;
        }
        std::vector<double> v;
        for (const auto& el : it.value()) {
            if (el.is_number()) {
                v.push_back(el.get<double>());
            }
        }
        if (!v.empty()) {
            out[it.key()] = std::move(v);
        }
    }
}

bool build_symbol_table(symbol_table_t& sym,
                        std::map<std::string, double>& scalars,
                        std::map<std::string, std::vector<double>>& vectors,
                        const json& j,
                        json& warnings) {
    sym.add_constants();
    if (j.contains("constants") && j["constants"].is_object()) {
        for (auto it = j["constants"].begin(); it != j["constants"].end(); ++it) {
            if (!it.value().is_number()) {
                continue;
            }
            const double val = it.value().get<double>();
            if (!sym.add_constant(it.key(), val)) {
                warnings.push_back("constant_ignored:" + it.key());
            }
        }
    }
    merge_number_object(j.contains("variables") ? j["variables"] : json::object(), scalars);
    merge_vectors_object(j.contains("vectors") ? j["vectors"] : json::object(), vectors);

    for (auto& p : scalars) {
        if (!sym.add_variable(p.first, p.second)) {
            return false;
        }
    }
    for (auto& p : vectors) {
        if (!sym.add_vector(p.first, p.second)) {
            return false;
        }
    }
    return true;
}

void setup_parser(parser_t& parser, ExprConfig cfg, ExprLoopRtc& rtc) {
    rtc.set_max(static_cast<std::uint64_t>(cfg.max_loop_iters));
    parser.register_loop_runtime_check(rtc);
    parser.settings().set_max_stack_depth(static_cast<std::size_t>(cfg.parser_stack_depth));
    parser.settings().set_max_node_depth(static_cast<std::size_t>(cfg.parser_node_depth));
    if (cfg.disable_control_flow) {
        parser.settings().disable_all_control_structures();
    }
    parser.disable_unknown_symbol_resolver();
}

std::size_t parser_compile_options() {
    return settings_t::default_compile_all_opts + settings_t::e_collect_vars +
           settings_t::e_collect_funcs + settings_t::e_collect_assings;
}

json results_to_json(const expression_t& expr) {
    json arr = json::array();
    const auto& rc = expr.results();
    using ts_t = typename exprtk::type_store<double>;
    for (std::size_t i = 0; i < rc.count(); ++i) {
        const ts_t& t = rc[i];
        json item;
        if (t.type == ts_t::e_scalar) {
            double v = 0;
            if (rc.get_scalar(i, v)) {
                item["type"] = "scalar";
                item["value"] = v;
            }
        } else if (t.type == ts_t::e_vector) {
            std::vector<double> vv;
            if (rc.get_vector(i, vv)) {
                item["type"] = "vector";
                item["value"] = vv;
            }
        } else if (t.type == ts_t::e_string) {
            std::string s;
            if (rc.get_string(i, s)) {
                item["type"] = "string";
                item["value"] = s;
            }
        }
        if (!item.empty()) {
            arr.push_back(std::move(item));
        }
    }
    return arr;
}

json expr_eval_invoke(const json& j, const ExprConfig& cfg) {
    if (!j.contains("expression") || !j["expression"].is_string()) {
        return make_expr_error("invalid_arguments", "missing or invalid expression");
    }
    const std::string program = j["expression"].get<std::string>();
    if (program.size() > cfg.max_expr_bytes) {
        return make_expr_error("expr_too_large", "expression exceeds AGENT_EXPR_MAX_EXPR_BYTES");
    }

    symbol_table_t sym(symbol_table_t::symtab_mutability_type::e_immutable);
    std::map<std::string, double> scalars;
    std::map<std::string, std::vector<double>> vectors;
    json warnings = json::array();
    if (!build_symbol_table(sym, scalars, vectors, j, warnings)) {
        return make_expr_error("invalid_arguments", "failed to register symbols");
    }

    ExprLoopRtc rtc;
    parser_t parser(parser_compile_options());
    setup_parser(parser, cfg, rtc);
    expression_t expression;
    expression.register_symbol_table(sym);
    if (!parser.compile(program, expression)) {
        return parse_error_json(parser);
    }

    try {
        const double v = expression.value();
        json out;
        out["expression"] = program;
        out["warnings"] = warnings;
        if (expression.return_invoked()) {
            out["returned"] = true;
            out["value"] = nullptr;
            out["results"] = results_to_json(expression);
            const std::string rf =
                j.contains("return_format") && j["return_format"].is_string()
                    ? j["return_format"].get<std::string>()
                    : "scalar";
            if (rf == "full") {
                out["return_format"] = "full";
            }
        } else {
            out["returned"] = false;
            if (!std::isfinite(v)) {
                return make_expr_error("non_finite_result", "result is NaN or Infinity");
            }
            out["value"] = v;
            const std::string rf =
                j.contains("return_format") && j["return_format"].is_string()
                    ? j["return_format"].get<std::string>()
                    : "scalar";
            if (rf == "full") {
                out["results"] = json::array();
            }
        }
        return out;
    } catch (const std::runtime_error& e) {
        const char* what = e.what();
        if (what && std::string(what) == "exprtk_loop_limit") {
            return make_expr_error("loop_limit", "loop iteration limit exceeded");
        }
        return make_expr_error("tool_internal_error", truncate_msg(what ? what : "runtime_error", 256));
    }
}

json expr_validate_invoke(const json& j, const ExprConfig& cfg) {
    if (!j.contains("expression") || !j["expression"].is_string()) {
        return make_expr_error("invalid_arguments", "missing or invalid expression");
    }
    const std::string program = j["expression"].get<std::string>();
    if (program.size() > cfg.max_expr_bytes) {
        return make_expr_error("expr_too_large", "expression exceeds AGENT_EXPR_MAX_EXPR_BYTES");
    }

    symbol_table_t sym(symbol_table_t::symtab_mutability_type::e_immutable);
    std::map<std::string, double> scalars;
    std::map<std::string, std::vector<double>> vectors;
    json warnings = json::array();
    if (!build_symbol_table(sym, scalars, vectors, j, warnings)) {
        return make_expr_error("invalid_arguments", "failed to register symbols");
    }

    ExprLoopRtc rtc;
    parser_t parser(parser_compile_options());
    setup_parser(parser, cfg, rtc);
    expression_t expression;
    expression.register_symbol_table(sym);

    parser.dec().collect_variables() = true;
    parser.dec().collect_functions() = true;
    parser.dec().collect_assignments() = true;

    if (!parser.compile(program, expression)) {
        return parse_error_json(parser);
    }

    std::deque<symbol_t> sym_list;
    parser.dec().symbols(sym_list);
    std::deque<symbol_t> assign_list;
    parser.dec().assignment_symbols(assign_list);

    json vars = json::array();
    json funcs = json::array();
    for (const auto& p : sym_list) {
        json item = {{"name", p.first}, {"kind", symbol_kind_str(p.second)}};
        if (p.second == parser_t::symbol_type::e_st_function) {
            funcs.push_back(std::move(item));
        } else {
            vars.push_back(std::move(item));
        }
    }
    json assigns = json::array();
    for (const auto& p : assign_list) {
        assigns.push_back(json{{"name", p.first}, {"kind", symbol_kind_str(p.second)}});
    }

    return json{{"ok", true},
                {"expression", program},
                {"variables", std::move(vars)},
                {"functions", std::move(funcs)},
                {"assignments", std::move(assigns)},
                {"warnings", std::move(warnings)}};
}

json expr_batch_eval_invoke(const json& j, const ExprConfig& cfg) {
    if (!j.contains("expression") || !j["expression"].is_string()) {
        return make_expr_error("invalid_arguments", "missing or invalid expression");
    }
    if (!j.contains("rows") || !j["rows"].is_array() || j["rows"].empty()) {
        return make_expr_error("invalid_arguments", "rows must be a non-empty array");
    }
    const std::string program = j["expression"].get<std::string>();
    if (program.size() > cfg.max_expr_bytes) {
        return make_expr_error("expr_too_large", "expression exceeds AGENT_EXPR_MAX_EXPR_BYTES");
    }

    std::set<std::string> scalar_keys;
    if (j.contains("variables") && j["variables"].is_object()) {
        for (auto it = j["variables"].begin(); it != j["variables"].end(); ++it) {
            if (it.value().is_number()) {
                scalar_keys.insert(it.key());
            }
        }
    }
    for (const auto& row : j["rows"]) {
        if (!row.is_object() || !row.contains("variables") || !row["variables"].is_object()) {
            continue;
        }
        for (auto it = row["variables"].begin(); it != row["variables"].end(); ++it) {
            if (it.value().is_number()) {
                scalar_keys.insert(it.key());
            }
        }
    }
    if (scalar_keys.empty()) {
        return make_expr_error("invalid_arguments", "no scalar variables in variables/rows");
    }

    json base_j = j;
    base_j["variables"] = j.contains("variables") ? j["variables"] : json::object();
    symbol_table_t sym(symbol_table_t::symtab_mutability_type::e_immutable);
    std::map<std::string, double> scalars;
    std::map<std::string, std::vector<double>> vectors;
    json warnings = json::array();
    for (const std::string& k : scalar_keys) {
        scalars[k] = 0.0;
    }
    merge_number_object(base_j["variables"], scalars);
    merge_vectors_object(j.contains("vectors") ? j["vectors"] : json::object(), vectors);
    if (!build_symbol_table(sym, scalars, vectors, j, warnings)) {
        return make_expr_error("invalid_arguments", "failed to register symbols");
    }

    ExprLoopRtc rtc;
    parser_t parser(parser_compile_options());
    setup_parser(parser, cfg, rtc);
    expression_t expression;
    expression.register_symbol_table(sym);
    if (!parser.compile(program, expression)) {
        return parse_error_json(parser);
    }

    json values = json::array();
    for (const auto& row : j["rows"]) {
        if (!row.is_object()) {
            return make_expr_error("invalid_arguments", "each row must be an object");
        }
        std::map<std::string, double> row_scalars = scalars;
        if (row.contains("variables") && row["variables"].is_object()) {
            merge_number_object(row["variables"], row_scalars);
        }
        for (const auto& k : scalar_keys) {
            auto it = row_scalars.find(k);
            if (it == row_scalars.end()) {
                return make_expr_error("invalid_arguments", "missing variable for row: " + k);
            }
            scalars[k] = it->second;
        }
        try {
            const double v = expression.value();
            if (expression.return_invoked()) {
                return make_expr_error("invalid_arguments", "batch_eval does not support return()");
            }
            if (!std::isfinite(v)) {
                return make_expr_error("non_finite_result", "non-finite value in batch row");
            }
            values.push_back(v);
        } catch (const std::runtime_error& e) {
            const char* what = e.what();
            if (what && std::string(what) == "exprtk_loop_limit") {
                return make_expr_error("loop_limit", "loop iteration limit exceeded");
            }
            return make_expr_error("tool_internal_error", truncate_msg(what ? what : "runtime_error", 256));
        }
    }

    return json{{"values", std::move(values)}, {"expression", program}, {"warnings", warnings}};
}

json expr_schema_expression_field(std::size_t max_expr_bytes) {
    return json{{"type", "string"},
                {"maxLength", max_expr_bytes},
                {"description",
                 "ExprTk program (UTF-8). Single expression or multiple statements separated by ';'. "
                 "maxLength matches runtime cap AGENT_EXPR_MAX_EXPR_BYTES. "
                 "Trig expects radians unless you convert (e.g. deg2rad). "
                 "Unknown identifiers fail at compile time (error codes parse_error / undefined_symbol)."}};
}

json expr_schema_variables_field(const char* role_suffix) {
    std::string desc =
        "Object map of scalar names to JSON numbers. Each key becomes a mutable double variable in the "
        "symbol table before compile. External symbols are not assignable with ':=' from the program "
        "(immutable symbol table). ";
    desc += role_suffix;
    desc += " Values must be JSON numbers (ToolBus schema subset allows additionalProperties: true only).";
    return json{{"type", "object"},
                {"additionalProperties", true},
                {"description", std::move(desc)}};
}

json expr_schema_vectors_field(const char* role_suffix) {
    std::string desc =
        "Object map of vector names to JSON arrays of numbers. Each array is loaded as an ExprTk vector; "
        "index with name[i] in the program. ";
    desc += role_suffix;
    desc += " Each value must be a JSON array whose elements are numbers.";
    return json{{"type", "object"},
                {"additionalProperties", true},
                {"description", std::move(desc)}};
}

json expr_schema_constants_field(const char* role_suffix) {
    std::string desc =
        "Object map of constant names to JSON numbers. Registered as read-only before variables. "
        "If a name cannot be added, a warning constant_ignored:<name> is emitted. ";
    desc += role_suffix;
    desc += " Values must be JSON numbers.";
    return json{{"type", "object"},
                {"additionalProperties", true},
                {"description", std::move(desc)}};
}

json expr_batch_row_item_schema() {
    json vars_in_row = json::object();
    vars_in_row["type"] = "object";
    vars_in_row["additionalProperties"] = true;
    vars_in_row["description"] =
        "Scalars to merge for this row before evaluation. May be omitted entirely to repeat the previous "
        "row's numbers.";
    json row = json::object();
    row["type"] = "object";
    row["description"] =
        "Optional per-row scalar overrides. Omitted keys keep values carried from the previous row "
        "(after that row's eval) or from top-level variables / 0.0 defaults for names only seen in later "
        "rows.";
    row["properties"] = json{{"variables", std::move(vars_in_row)}};
    return row;
}

json expr_eval_tool_schema(const ExprConfig& cfg) {
    json return_fmt = json::object();
    return_fmt["type"] = "string";
    return_fmt["enum"] = json::array({"scalar", "full"});
    return_fmt["default"] = "scalar";
    return_fmt["description"] =
        "scalar (default): normal path returns numeric value in value; if the program invokes return(), "
        "value is null and results lists typed values. full: when return() is used, also sets "
        "return_format in the response; when no return(), results may be an empty array for symmetry "
        "with tooling.";
    json props = json::object();
    props["expression"] = expr_schema_expression_field(cfg.max_expr_bytes);
    props["variables"] =
        expr_schema_variables_field("Use for inputs that appear as scalar symbols.");
    props["vectors"] = expr_schema_vectors_field("Shared numeric vectors for this evaluation.");
    props["constants"] = expr_schema_constants_field("Shared constants for this evaluation.");
    props["return_format"] = std::move(return_fmt);
    json s = json::object();
    s["type"] = "object";
    s["description"] = "Evaluate one ExprTk program once with the given symbol bindings.";
    s["properties"] = std::move(props);
    s["required"] = json::array({"expression"});
    return s;
}

json expr_validate_tool_schema(const ExprConfig& cfg) {
    json props = json::object();
    props["expression"] = expr_schema_expression_field(cfg.max_expr_bytes);
    props["variables"] =
        expr_schema_variables_field("Ensures names used as scalars resolve during compile.");
    props["vectors"] = expr_schema_vectors_field("Ensures vector symbols exist during compile.");
    props["constants"] =
        expr_schema_constants_field("Ensures constant names match expr_eval setup.");
    json s = json::object();
    s["type"] = "object";
    s["description"] = "Parse-check only; same binding shape as expr_eval (no return_format).";
    s["properties"] = std::move(props);
    s["required"] = json::array({"expression"});
    return s;
}

json expr_batch_eval_tool_schema(const ExprConfig& cfg) {
    json rows_field = json::object();
    rows_field["type"] = "array";
    rows_field["minItems"] = 1;
    rows_field["description"] =
        "Ordered list of row objects. Length equals length of returned values.";
    rows_field["items"] = expr_batch_row_item_schema();
    json props = json::object();
    props["expression"] = expr_schema_expression_field(cfg.max_expr_bytes);
    props["variables"] = expr_schema_variables_field(
        "Default scalars for all rows; keys also define which symbols participate in the batch unless "
        "only rows supply names.");
    props["vectors"] = expr_schema_vectors_field("Same vector bindings for every row evaluation.");
    props["constants"] = expr_schema_constants_field("Same constant bindings for every row evaluation.");
    props["rows"] = std::move(rows_field);
    json s = json::object();
    s["type"] = "object";
    s["description"] =
        "Batch scalar evaluation with one compile; see tool description string for row merge rules.";
    s["properties"] = std::move(props);
    s["required"] = json::array({"expression", "rows"});
    return s;
}

void register_expr_tools_impl(ToolBus& bus) {
    const ExprConfig cfg = load_expr_config_from_env();

    {
        ToolMeta meta;
        meta.name = "Calculate";
        meta.description =
            "Evaluate an ExprTk program (IEEE double). "
            "Arguments: expression (required); optional variables (mutable scalars), vectors (name -> "
            "number[]), constants (read-only scalars); optional return_format (scalar|full). "
            "Outputs: value when the program yields a scalar without return(); returned/results when "
            "return() is used; warnings for skipped constants. "
            "Limits: AGENT_EXPR_MAX_LOOP_ITERS, parser stack/node depth env vars; optional "
            "AGENT_EXPR_DISABLE_CONTROL_FLOW. External symbols are immutable (no ':=' on them).";
        meta.schema = expr_eval_tool_schema(cfg);
        meta.side_effect = ToolSideEffect::ReadOnly;
        bus.register_local_tool(
            "Calculate", [cfg](const json& args) { return expr_eval_invoke(args, cfg); }, meta);
    }
    {
        ToolMeta meta;
        meta.name = "ValidateExpression";
        meta.description =
            "Parse-only: compile the ExprTk program with the same variables/vectors/constants bindings "
            "as expr_eval, but do not run value(). Returns ok, variables/functions/assignments symbol "
            "lists, and warnings. Use to discover free symbols and arity before calling expr_eval or "
            "expr_batch_eval.";
        meta.schema = expr_validate_tool_schema(cfg);
        meta.side_effect = ToolSideEffect::ReadOnly;
        bus.register_local_tool(
            "ValidateExpression", [cfg](const json& args) { return expr_validate_invoke(args, cfg); }, meta);
    }
    {
        ToolMeta meta;
        meta.name = "BatchCalculate";
        meta.description =
            "Compile expression once, then call expression.value() once per rows[] entry. "
            "Scalar set: union of every key appearing in top-level variables and in any rows[i].variables "
            "(must be non-empty). Top-level variables seed defaults; vectors and constants are shared. "
            "Per row: start from the resolved scalars after the previous row, merge rows[i].variables, "
            "then evaluate. "
            "Unsupported: return() (fails). Any non-finite value or loop-limit error fails the entire call. "
            "Response values[] aligns 1:1 with rows[].";
        meta.schema = expr_batch_eval_tool_schema(cfg);
        meta.side_effect = ToolSideEffect::ReadOnly;
        bus.register_local_tool(
            "BatchCalculate", [cfg](const json& args) { return expr_batch_eval_invoke(args, cfg); },
            meta);
    }
}

#endif // AGENT_HAVE_EXPRTK

} // namespace

void register_builtin_expr_tools_if_configured(ToolBus& bus) {
#if defined(AGENT_HAVE_EXPRTK) && AGENT_HAVE_EXPRTK
    if (bus.get_tool_info("Calculate").has_value()) {
        return;
    }
    if (!expr_register_enabled()) {
        return;
    }
    register_expr_tools_impl(bus);
    bus.register_tool_alias("expr_eval", "Calculate");
    bus.register_tool_alias("expr_validate", "ValidateExpression");
    bus.register_tool_alias("expr_batch_eval", "BatchCalculate");
#else
    (void)bus;
#endif
}

} // namespace agent_framework
