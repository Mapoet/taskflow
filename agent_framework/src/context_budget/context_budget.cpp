/**
 * @file context_budget.cpp
 * @brief WP2.1c 上下文预算：UTF-8、紧凑 dump、方案 S、spill、线路上限
 */

#include "agent/context_budget.hpp"

#include "agent/types.hpp"

#include <cctype>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <iostream>
#include <random>
#include <sstream>
#include <system_error>

namespace agent_framework {

namespace {

bool env_truthy(const char* v) {
    if (!v || !*v) {
        return false;
    }
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T';
}

void warn_parse(const char* name, const char* detail) {
    std::clog << "[context_budget] warn: invalid " << name << " (" << detail << "), using default\n";
}

std::optional<std::size_t> parse_u64_string(const char* s) {
    if (!s || !*s) {
        return std::nullopt;
    }
    char* end = nullptr;
    unsigned long long v = std::strtoull(s, &end, 10);
    if (end == s || *end != '\0' || v == ULLONG_MAX) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(v);
}

std::optional<std::size_t> json_to_u64(const json& j) {
    if (j.is_number_unsigned()) {
        return static_cast<std::size_t>(j.get<std::uint64_t>());
    }
    if (j.is_number_integer()) {
        const auto n = j.get<std::int64_t>();
        if (n < 0) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(n);
    }
    if (j.is_string()) {
        return parse_u64_string(j.get_ref<const std::string&>().c_str());
    }
    return std::nullopt;
}

std::optional<std::size_t> cfg_u64(const AgentConfig* cfg, const char* key) {
    if (!cfg) {
        return std::nullopt;
    }
    const auto it = cfg->extra_config.find(key);
    if (it == cfg->extra_config.end()) {
        return std::nullopt;
    }
    return json_to_u64(it->second);
}

std::optional<std::string> cfg_string(const AgentConfig* cfg, const char* key) {
    if (!cfg) {
        return std::nullopt;
    }
    const auto it = cfg->extra_config.find(key);
    if (it == cfg->extra_config.end()) {
        return std::nullopt;
    }
    if (!it->second.is_string()) {
        return std::nullopt;
    }
    return it->second.get<std::string>();
}

std::optional<bool> cfg_bool(const AgentConfig* cfg, const char* key) {
    if (!cfg) {
        return std::nullopt;
    }
    const auto it = cfg->extra_config.find(key);
    if (it == cfg->extra_config.end()) {
        return std::nullopt;
    }
    if (it->second.is_boolean()) {
        return it->second.get<bool>();
    }
    if (it->second.is_number_integer()) {
        return it->second.get<int>() != 0;
    }
    if (it->second.is_string()) {
        return env_truthy(it->second.get_ref<const std::string&>().c_str());
    }
    return std::nullopt;
}

void shrink_wrap_to_cap(const std::string& original_dump, std::size_t cap_bytes, AfTruncationKind kind,
                        bool spill_ok, const std::string& spill_ref, json& out) {
    if (cap_bytes < 64) {
        out = json{{"_af_truncation",
                    json{{"kind", af_truncation_kind_string(kind)},
                         {"original_utf8_bytes", original_dump.size()},
                         {"kept_utf8_bytes", static_cast<std::size_t>(0)},
                         {"spill", spill_ok},
                         {"ref", spill_ref}}},
                 {"preview_text", ""}};
        return;
    }
    std::size_t preview_budget = cap_bytes > 512 ? cap_bytes - 512 : 0;
    std::string preview = utf8_safe_truncate(original_dump, preview_budget);
    for (int iter = 0; iter < 48; ++iter) {
        AfTruncationMeta meta;
        meta.kind = kind;
        meta.original_utf8_bytes = original_dump.size();
        meta.kept_utf8_bytes = preview.size();
        meta.spill = spill_ok;
        meta.ref = spill_ref;
        out = wrap_truncated_payload(meta, preview);
        std::string d;
        try {
            d = out.dump(-1, ' ', false, json::error_handler_t::strict);
        } catch (...) {
            preview_budget = preview_budget * 3 / 4;
            preview = utf8_safe_truncate(original_dump, preview_budget);
            continue;
        }
        if (d.size() <= cap_bytes) {
            return;
        }
        preview_budget = preview_budget * 3 / 4;
        preview = utf8_safe_truncate(original_dump, preview_budget);
    }
    out = json{{"_af_truncation",
                json{{"kind", af_truncation_kind_string(kind)},
                     {"original_utf8_bytes", original_dump.size()},
                     {"kept_utf8_bytes", static_cast<std::size_t>(0)},
                     {"spill", spill_ok},
                     {"ref", spill_ref}}},
             {"preview_text", ""}};
}

} // namespace

const char* af_truncation_kind_string(AfTruncationKind kind) {
    switch (kind) {
    case AfTruncationKind::tool_result:
        return "tool_result";
    case AfTruncationKind::user_injection:
        return "user_injection";
    case AfTruncationKind::llm_context:
        return "llm_context";
    case AfTruncationKind::wire_payload:
        return "wire_payload";
    default:
        return "tool_result";
    }
}

std::string json_compact_dump(const json& j) {
    try {
        return j.dump(-1, ' ', false, json::error_handler_t::strict);
    } catch (...) {
        return std::string(R"({"error":"json_dump_failed"})");
    }
}

std::size_t json_utf8_dump_bytes(const json& j) {
    return json_compact_dump(j).size();
}

std::string utf8_safe_truncate(std::string_view utf8_text, std::size_t max_bytes) {
    if (utf8_text.size() <= max_bytes) {
        return std::string(utf8_text);
    }
    std::size_t n = max_bytes;
    while (n > 0 && (static_cast<unsigned char>(utf8_text[n - 1]) & 0xC0u) == 0x80u) {
        --n;
    }
    return std::string(utf8_text.substr(0, n));
}

json wrap_truncated_payload(const AfTruncationMeta& meta, std::string_view preview_utf8) {
    json t = json::object();
    t["kind"] = af_truncation_kind_string(meta.kind);
    t["original_utf8_bytes"] = meta.original_utf8_bytes;
    t["kept_utf8_bytes"] = meta.kept_utf8_bytes;
    t["spill"] = meta.spill;
    t["ref"] = meta.ref;
    if (!meta.reason.empty()) {
        t["reason"] = meta.reason;
    }
    return json{{"_af_truncation", std::move(t)}, {"preview_text", std::string(preview_utf8)}};
}

void apply_spill_if_configured(const ContextBudgetLimits& limits, std::string_view full_dump_utf8,
                               AfTruncationMeta& meta_in_out) {
    if (limits.spill_dir.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(limits.spill_dir, ec);
    if (ec) {
        meta_in_out.spill = false;
        meta_in_out.ref.clear();
        return;
    }
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<unsigned> dist(0, 15);
    std::ostringstream name;
    name << "af_ctx_" << now << "_";
    for (int i = 0; i < 8; ++i) {
        static const char* const kHex = "0123456789abcdef";
        name << kHex[dist(gen) % 16];
    }
    name << ".json";
    const std::string fname = name.str();
    const std::filesystem::path path = std::filesystem::path(limits.spill_dir) / fname;
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        meta_in_out.spill = false;
        meta_in_out.ref.clear();
        return;
    }
    out.write(full_dump_utf8.data(), static_cast<std::streamsize>(full_dump_utf8.size()));
    if (!out.good()) {
        meta_in_out.spill = false;
        meta_in_out.ref.clear();
        return;
    }
    meta_in_out.spill = true;
    meta_in_out.ref = fname;
}

bool apply_per_tool_result_budget(json& in_out, const ContextBudgetLimits& limits, AfTruncationKind kind,
                                  std::string* warn_out) {
    (void)warn_out;
    const std::size_t cap = limits.max_tool_result_json_bytes;
    std::string original_dump;
    try {
        original_dump = in_out.dump(-1, ' ', false, json::error_handler_t::strict);
    } catch (...) {
        in_out = json{{"error", "json_dump_failed"}};
        original_dump = json_compact_dump(in_out);
    }
    if (original_dump.size() <= cap) {
        return true;
    }
    AfTruncationMeta meta;
    meta.kind = kind;
    meta.original_utf8_bytes = original_dump.size();
    meta.spill = false;
    apply_spill_if_configured(limits, original_dump, meta);
    json wrapped;
    shrink_wrap_to_cap(original_dump, cap, kind, meta.spill, meta.ref, wrapped);
    in_out = std::move(wrapped);
    return json_utf8_dump_bytes(in_out) <= cap;
}

json combined_budget_exhausted_placeholder(std::size_t original_dump_bytes) {
    AfTruncationMeta meta;
    meta.kind = AfTruncationKind::tool_result;
    meta.original_utf8_bytes = original_dump_bytes;
    meta.kept_utf8_bytes = std::string("combined_budget_exhausted").size();
    meta.spill = false;
    meta.reason = "combined_budget_exhausted";
    return wrap_truncated_payload(meta, "combined_budget_exhausted");
}

bool is_combined_budget_stub(const json& tool_result) {
    if (!tool_result.is_object()) {
        return false;
    }
    const auto it = tool_result.find("_af_truncation");
    if (it == tool_result.end() || !it->is_object()) {
        return false;
    }
    const auto r = it->find("reason");
    return r != it->end() && r->is_string() && r->get<std::string>() == "combined_budget_exhausted";
}

bool apply_wire_payload_cap(json& in_out, std::size_t max_bytes, std::string* warn_out) {
    (void)warn_out;
    if (max_bytes == 0) {
        return true;
    }
    std::string d = json_compact_dump(in_out);
    if (d.size() <= max_bytes) {
        return true;
    }
    AfTruncationMeta meta;
    meta.kind = AfTruncationKind::wire_payload;
    meta.original_utf8_bytes = d.size();
    meta.spill = false;
    json wrapped;
    shrink_wrap_to_cap(d, max_bytes, AfTruncationKind::wire_payload, false, "", wrapped);
    in_out = std::move(wrapped);
    return json_utf8_dump_bytes(in_out) <= max_bytes;
}

ContextBudgetLimits ContextBudgetLimits::load(const AgentConfig* agent_cfg) {
    ContextBudgetLimits L;
    auto apply_u64 = [&](const char* env_key, const char* cfg_key, std::size_t& field,
                         std::size_t default_val) {
        std::size_t v = default_val;
        if (const auto c = cfg_u64(agent_cfg, cfg_key)) {
            v = *c;
        } else if (const char* e = std::getenv(env_key)) {
            if (const auto p = parse_u64_string(e)) {
                v = *p;
            } else {
                warn_parse(env_key, "not a non-negative integer");
                v = default_val;
            }
        }
        field = v;
    };

    apply_u64("AGENT_BUDGET_MAX_TOOL_RESULT_JSON_BYTES", "BUDGET_MAX_TOOL_RESULT_JSON_BYTES",
              L.max_tool_result_json_bytes, 1048576);
    apply_u64("AGENT_BUDGET_MAX_INJECTION_BYTES", "BUDGET_MAX_INJECTION_BYTES", L.max_injection_bytes,
              2097152);
    apply_u64("AGENT_BUDGET_MAX_COMBINED_PROMPT_ATTACH_BYTES", "BUDGET_MAX_COMBINED_PROMPT_ATTACH_BYTES",
              L.max_combined_prompt_attach_bytes, 8388608);
    apply_u64("AGENT_BUDGET_MAX_RENDERED_MESSAGES_BYTES", "BUDGET_MAX_RENDERED_MESSAGES_BYTES",
              L.max_rendered_messages_bytes, 0);
    apply_u64("AGENT_BUDGET_MAX_WIRE_MESSAGE_BYTES", "BUDGET_MAX_WIRE_MESSAGE_BYTES",
              L.max_wire_message_bytes, 4194304);

    if (const auto cs = cfg_string(agent_cfg, "CONTEXT_BUDGET_SPILL_DIR")) {
        L.spill_dir = *cs;
    } else if (const char* e = std::getenv("AGENT_CONTEXT_BUDGET_SPILL_DIR")) {
        L.spill_dir = e;
    }

    if (const auto cb = cfg_bool(agent_cfg, "CONTEXT_BUDGET_STRICT")) {
        L.context_budget_strict = *cb;
    } else {
        L.context_budget_strict = env_truthy(std::getenv("AGENT_CONTEXT_BUDGET_STRICT"));
    }

    return L;
}

} // namespace agent_framework
