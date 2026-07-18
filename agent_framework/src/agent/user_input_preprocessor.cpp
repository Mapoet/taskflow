/**
 * @file user_input_preprocessor.cpp
 * @brief WP2.7 UserInputPreprocessor 实现
 */

#include <agent/agent/user_input_preprocessor.hpp>

#include <agent/context_budget/context_budget.hpp>
#include <agent/toolbus/fs_sandbox.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/agent/memory_compaction.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/resources/resource_uri.hpp>
#include <agent/resources/session_resource_context.hpp>
#include <agent/skills/skill_loader.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <regex>
#include <sstream>

namespace agent_framework {

namespace {

bool env_truthy(const char* v) {
    if (!v || !*v) {
        return false;
    }
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T';
}

std::string trim_copy(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    std::size_t j = s.size();
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) {
        --j;
    }
    return std::string(s.substr(i, j - i));
}

std::string truncate_utf8(std::string_view s, std::size_t max_chars) {
    return utf8_safe_truncate(s, max_chars);
}

std::string injection_header(std::string_view kind, std::string_view ref_trunc) {
    return std::string("\n--- injection:") + std::string(kind) + ":" + std::string(ref_trunc) + "\n";
}

std::size_t file_inject_max_bytes() {
    const char* e = std::getenv("AGENT_INPUT_FILE_INJECT_MAX_BYTES");
    if (!e || !*e) {
        return 262144;
    }
    const long v = std::strtol(e, nullptr, 10);
    if (v <= 0) {
        return 262144;
    }
    return static_cast<std::size_t>(v);
}

bool is_cmd_line(std::string_view line_trimmed) {
    return !line_trimmed.empty() && line_trimmed[0] == '/';
}

/** @return false if not a whitelist cmd (strict adds violation elsewhere) */
bool parse_cmd_line(const std::string& line_trimmed, bool strict, const ExecutionContext& ctx,
                    ProcessedUserInput& out, std::vector<std::string>& lines_kept) {
    std::istringstream iss(line_trimmed);
    std::vector<std::string> tok;
    std::string w;
    while (iss >> w) {
        tok.push_back(w);
    }
    if (tok.empty()) {
        return true;
    }
    std::string t0 = tok[0];
    for (char& c : t0) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (!t0.empty() && t0[0] == '/') {
        t0 = t0.substr(1);
    }
    if (t0 == "memory" && tok.size() >= 2) {
        std::string t1 = tok[1];
        for (char& c : t1) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (t1 == "compact") {
            ControlAction a;
            a.command = "memory.compact";
            a.args = json::object();
            a.raw_line = line_trimmed;
            out.control_actions.push_back(std::move(a));
            std::clog << "[user_command] OK policy=" << ctx.input_policy_version << " cmd=memory.compact"
                      << (ctx.session_id ? " session=" + *ctx.session_id : "") << "\n";
            return true;
        }
        if (t1 == "clear") {
            ControlAction a;
            a.command = "memory.clear";
            a.args = json::object();
            a.raw_line = line_trimmed;
            out.control_actions.push_back(std::move(a));
            std::clog << "[user_command] OK policy=" << ctx.input_policy_version << " cmd=memory.clear"
                      << (ctx.session_id ? " session=" + *ctx.session_id : "") << "\n";
            return true;
        }
    }
    if (t0 == "model" && tok.size() >= 2) {
        std::string id;
        for (std::size_t k = 1; k < tok.size(); ++k) {
            if (k > 1) {
                id += ' ';
            }
            id += tok[k];
        }
        id = trim_copy(id);
        if (id.empty()) {
            if (strict) {
                out.tier_a_violations.push_back("command_invalid_args:/model");
            }
            std::clog << "[user_command] REJECTED policy=" << ctx.input_policy_version
                      << " stub=wp3.7_only line=" << truncate_utf8(line_trimmed, 200) << "\n";
            return true;
        }
        ControlAction a;
        a.command = "model.set";
        a.args = json{{"id", id}};
        a.raw_line = line_trimmed;
        out.control_actions.push_back(std::move(a));
        std::clog << "[user_command] OK policy=" << ctx.input_policy_version << " cmd=model.set"
                  << (ctx.session_id ? " session=" + *ctx.session_id : "") << "\n";
        return true;
    }
    if (t0 == "skills") {
        auto invalid = [&]() {
            if (strict) out.tier_a_violations.push_back("command_invalid_args:/skills");
            return true;
        };
        if (tok.size() < 2) return invalid();
        std::string operation = tok[1];
        for (char& c : operation) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        ControlAction action;
        action.command = "skills." + operation;
        action.raw_line = line_trimmed;
        if (operation == "list" || operation == "status" || operation == "reload" ||
            operation == "deactivate") {
            if (tok.size() != 2) return invalid();
        } else if (operation == "activate" || operation == "validate") {
            if (tok.size() != 3) return invalid();
            action.args["id"] = tok[2];
        } else if (operation == "create") {
            if (tok.size() < 3) return invalid();
            action.args["id"] = tok[2];
            std::string description;
            if (tok.size() > 3) {
                if (tok[3] != "--description" || tok.size() < 5) return invalid();
                for (std::size_t i = 4; i < tok.size(); ++i) {
                    if (!description.empty()) description.push_back(' ');
                    description += tok[i];
                }
                if (description.size() >= 2 &&
                    ((description.front() == '"' && description.back() == '"') ||
                     (description.front() == '\'' && description.back() == '\''))) {
                    description = description.substr(1, description.size() - 2);
                }
            }
            action.args["description"] = description;
        } else {
            return invalid();
        }
        out.control_actions.push_back(std::move(action));
        std::clog << "[user_command] OK policy=" << ctx.input_policy_version
                  << " cmd=skills." << operation << "\n";
        return true;
    }

    if (strict) {
        out.tier_a_violations.push_back(
            std::string("command_not_whitelisted:") + truncate_utf8(line_trimmed, 200));
        std::clog << "[user_command] REJECTED policy=" << ctx.input_policy_version
                  << " stub=wp3.7_only line=" << truncate_utf8(line_trimmed, 200)
                  << (ctx.session_id ? " session=" + *ctx.session_id : "") << "\n";
    } else {
        std::clog << "[user_command] REJECTED(relaxed) policy=" << ctx.input_policy_version
                  << " line=" << truncate_utf8(line_trimmed, 200) << "\n";
    }
    (void)lines_kept;
    return true;
}

std::string strip_cmd_lines(std::string_view raw, bool strict, const ExecutionContext& ctx,
                            ProcessedUserInput& out) {
    std::string s(raw);
    std::vector<std::string> lines;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    lines.push_back(cur);

    std::ostringstream rebuilt;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string lt = trim_copy(lines[i]);
        if (is_cmd_line(lt)) {
            std::vector<std::string> dummy;
            parse_cmd_line(lt, strict, ctx, out, dummy);
            continue;
        }
        if (i > 0) {
            rebuilt << '\n';
        }
        rebuilt << lines[i];
    }
    return rebuilt.str();
}

void collapse_blank_lines(std::string& s) {
    std::regex re(R"(\n{3,})");
    s = std::regex_replace(s, re, "\n\n");
}

bool match_range(const std::string& inner, std::string& path_out, std::optional<std::string>& range_out) {
    const auto last_comma = inner.find_last_of(',');
    if (last_comma == std::string::npos) {
        path_out = trim_copy(inner);
        if (path_out.empty()) {
            return false;
        }
        range_out.reset();
        return true;
    }
    const std::string left = trim_copy(inner.substr(0, last_comma));
    const std::string right = trim_copy(inner.substr(last_comma + 1));
    static const std::regex range_re(R"(^\d+-\d+$)");
    if (!std::regex_match(right, range_re) || left.empty()) {
        return false;
    }
    path_out = left;
    range_out = right;
    return true;
}

std::string apply_line_range_str(std::string content, std::string_view range_expr) {
    const auto dash = range_expr.find('-');
    if (dash == std::string::npos) {
        return content;
    }
    const int lo = std::stoi(std::string(range_expr.substr(0, dash)));
    const int hi = std::stoi(std::string(range_expr.substr(dash + 1)));
    if (lo < 1 || hi < lo) {
        return {};
    }
    std::vector<std::string> lines;
    std::string cur;
    for (char c : content) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    lines.push_back(cur);
    std::ostringstream out;
    for (int ln = lo; ln <= hi && ln <= static_cast<int>(lines.size()); ++ln) {
        if (ln > lo) {
            out << '\n';
        }
        out << lines[static_cast<std::size_t>(ln - 1)];
    }
    return out.str();
}

std::string tool_err_summary(const json& r, std::size_t cap) {
    try {
        if (r.contains("error") && r["error"].is_object()) {
            const auto& e = r["error"];
            std::string msg = e.value("message", e.dump());
            return truncate_utf8(msg, cap);
        }
    } catch (...) {
    }
    return "tool_error";
}

class InjectionByteTracker {
public:
    explicit InjectionByteTracker(std::size_t max_inj_bytes) : limit_(max_inj_bytes) {}

    bool try_consume(std::string_view header_utf8, std::size_t block_bytes) {
        const std::size_t add = header_utf8.size() + block_bytes;
        if (used_ + add > limit_) {
            return false;
        }
        used_ += add;
        return true;
    }

private:
    std::size_t limit_;
    std::size_t used_ = 0;
};

std::string extract_fs_read_text(const json& r) {
    if (r.contains("error")) {
        return {};
    }
    if (r.contains("content") && r["content"].is_string()) {
        return r["content"].get<std::string>();
    }
    return {};
}

std::string extract_web_fetch_text(const json& r) {
    if (r.contains("error")) {
        return {};
    }
    if (r.contains("text") && r["text"].is_string()) {
        return r["text"].get<std::string>();
    }
    if (r.contains("json")) {
        return r["json"].dump();
    }
    if (r.contains("html") && r["html"].is_string()) {
        return r["html"].get<std::string>();
    }
    if (r.contains("binary_preview_hex") && r["binary_preview_hex"].is_string()) {
        return std::string("[binary] ") + r["binary_preview_hex"].get<std::string>();
    }
    return {};
}

std::optional<json> json_from_llm_answer(std::string raw) {
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) {
        raw.erase(raw.begin());
    }
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) {
        raw.pop_back();
    }
    if (raw.size() >= 7 && raw.compare(0, 7, "```json") == 0) {
        raw = raw.substr(7);
    } else if (raw.size() >= 3 && raw.compare(0, 3, "```") == 0) {
        raw = raw.substr(3);
    }
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) {
        raw.erase(raw.begin());
    }
    const auto fence = raw.rfind("```");
    if (fence != std::string::npos) {
        raw = raw.substr(0, fence);
    }
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) {
        raw.pop_back();
    }
    try {
        return json::parse(raw);
    } catch (...) {
        return std::nullopt;
    }
}

enum class TierBResolveResult { Resolved, Failed };

TierBResolveResult tier_b_try_resolve(std::string_view fragment, const ExecutionContext& ctx, ProcessedUserInput& out,
                        InjectionByteTracker& tracker, const PreprocessOptions& opt,
                        const std::optional<FsSandboxConfig>& cfg_opt) {
    if (!opt.enable_tier_b || !opt.tier_b_llm) {
        return TierBResolveResult::Failed;
    }
    LLMInput in;
    in.system_prompt =
        "You disambiguate user input. Output ONLY one JSON object, no markdown.\n"
        "Schema: {\"action\":\"ignore\"|\"inject_file\"|\"inject_url\",\"path_or_url\":\"string\","
        "\"reason\":\"string\"}\n"
        "Choose ignore unless a concrete file path or https URL is clearly intended.";
    in.user_prompt = std::string("Fragment:\n").append(fragment);
    std::future<LLMOutput> fut;
    try {
        fut = opt.tier_b_llm->invoke(in, "");
    } catch (...) {
        return TierBResolveResult::Failed;
    }
    if (fut.wait_for(std::chrono::milliseconds(std::max(1, opt.tier_b_timeout_ms))) !=
        std::future_status::ready) {
        return TierBResolveResult::Failed;
    }
    LLMOutput lo;
    try {
        lo = fut.get();
    } catch (...) {
        return TierBResolveResult::Failed;
    }
    const auto j = json_from_llm_answer(lo.final_answer.empty() ? lo.reasoning : lo.final_answer);
    if (!j) {
        return TierBResolveResult::Failed;
    }
    const std::string act = j->value("action", "");
    if (act == "ignore") {
        return TierBResolveResult::Resolved;
    }
    const std::string pu = j->value("path_or_url", "");
    if (pu.empty() || !opt.toolbus) {
        return TierBResolveResult::Failed;
    }
    if (act == "inject_file") {
        if (!cfg_opt) {
            return TierBResolveResult::Failed;
        }
        namespace fs = std::filesystem;
        fs::path raw_path(pu);
        fs::path combined = raw_path.is_absolute() ? raw_path : fs::path(ctx.cwd) / raw_path;
        std::error_code ec;
        fs::path canon = fs::weakly_canonical(combined, ec);
        if (ec) {
            return TierBResolveResult::Failed;
        }
        json path_err = json::object();
        auto resolved = fs_resolve_under_root(canon.string(), cfg_opt->root, path_err);
        if (!resolved) {
            return TierBResolveResult::Failed;
        }
        std::size_t cap_read = cfg_opt->max_read_bytes;
        if (const std::size_t inj_cap = file_inject_max_bytes()) {
            cap_read = std::min(cap_read, inj_cap);
        }
        json fr = opt.toolbus
                      ->call_tool("fs_read", json{{"path", resolved->string()}, {"max_bytes", cap_read}})
                      .get();
        std::string text = extract_fs_read_text(fr);
        if (text.empty() && fr.contains("error")) {
            return TierBResolveResult::Failed;
        }
        if (text.size() > file_inject_max_bytes()) {
            return TierBResolveResult::Failed;
        }
        const std::string ref_trunc = truncate_utf8(pu, 200);
        const std::string hdr = injection_header("file", ref_trunc);
        if (!tracker.try_consume(hdr, text.size())) {
            return TierBResolveResult::Failed;
        }
        InjectedContextBlock blk;
        blk.source_kind = "file";
        blk.source_ref = pu;
        blk.text_utf8 = std::move(text);
        blk.byte_length = blk.text_utf8.size();
        out.injected_context.push_back(std::move(blk));
        return TierBResolveResult::Resolved;
    }
    if (act == "inject_url") {
        json wr = opt.toolbus->call_tool("web_fetch", json{{"url", pu}}).get();
        std::string text = extract_web_fetch_text(wr);
        if (text.empty() && wr.contains("error")) {
            return TierBResolveResult::Failed;
        }
        const std::string ref_trunc = truncate_utf8(pu, 200);
        const std::string hdr = injection_header("url", ref_trunc);
        if (!tracker.try_consume(hdr, text.size())) {
            return TierBResolveResult::Failed;
        }
        InjectedContextBlock blk;
        blk.source_kind = "url";
        blk.source_ref = pu;
        blk.text_utf8 = std::move(text);
        blk.byte_length = blk.text_utf8.size();
        out.injected_context.push_back(std::move(blk));
        return TierBResolveResult::Resolved;
    }
    return TierBResolveResult::Failed;
}

} // namespace

bool env_input_strict_enabled() {
    const char* v = std::getenv("AGENT_INPUT_STRICT");
    if (v == nullptr || v[0] == '\0') {
        return true;
    }
    return env_truthy(v);
}

bool env_input_tier_b_enabled() {
    return env_truthy(std::getenv("AGENT_INPUT_TIER_B"));
}

std::string concat_user_text_from_message(const AgentMessage& msg) {
    std::ostringstream oss;
    bool first = true;
    for (const auto& p : msg.parts) {
        if (p.type != AgentPart::Type::TEXT || !p.text || p.text->empty()) {
            continue;
        }
        if (!first) {
            oss << '\n';
        }
        first = false;
        oss << *p.text;
    }
    return oss.str();
}

void wp27_store_pending_in_task_metadata(const ProcessedUserInput& processed, const ExecutionContext& ctx,
                                         json& metadata_io) {
    json inj = json::array();
    for (const auto& b : processed.injected_context) {
        inj.push_back(json{{"source_kind", b.source_kind},
                           {"source_ref", b.source_ref},
                           {"mime_hint", b.mime_hint ? json(*b.mime_hint) : json(nullptr)},
                           {"text_utf8", b.text_utf8},
                           {"byte_length", b.byte_length}});
    }
    json ctr = json::array();
    for (const auto& c : processed.control_actions) {
        ctr.push_back(json{{"command", c.command}, {"args", c.args}, {"raw_line", c.raw_line}});
    }
    metadata_io["wp27_pending"] =
        json{{"injected_context", std::move(inj)},
             {"control_actions", std::move(ctr)},
             {"input_policy_version", ctx.input_policy_version},
             {"cwd", ctx.cwd},
             {"session_id", ctx.session_id ? json(*ctx.session_id) : json(nullptr)},
             {"task_id", ctx.task_id ? json(*ctx.task_id) : json(nullptr)}};
}

void wp27_restore_pending_from_task_metadata(const AgentTask& task, internal::AgentThreadState& st) {
    auto it = task.metadata.find("wp27_pending");
    if (it == task.metadata.end() || !it->is_object()) {
        return;
    }
    const json& w = *it;
    if (w.contains("injected_context") && w["injected_context"].is_array()) {
        for (const auto& item : w["injected_context"]) {
            InjectedContextBlock b;
            b.source_kind = item.value("source_kind", "");
            b.source_ref = item.value("source_ref", "");
            if (item.contains("mime_hint") && !item["mime_hint"].is_null()) {
                b.mime_hint = item["mime_hint"].get<std::string>();
            }
            b.text_utf8 = item.value("text_utf8", "");
            b.byte_length = item.value("byte_length", b.text_utf8.size());
            st.pending_injected_context.push_back(std::move(b));
        }
    }
    if (w.contains("control_actions") && w["control_actions"].is_array()) {
        for (const auto& item : w["control_actions"]) {
            ControlAction c;
            c.command = item.value("command", "");
            c.args = item.value("args", json::object());
            c.raw_line = item.value("raw_line", "");
            st.pending_control_actions.push_back(std::move(c));
        }
    }
    ExecutionContext ex;
    ex.input_policy_version = w.value("input_policy_version", "wp27-v1");
    ex.cwd = w.value("cwd", ex.cwd);
    if (w.contains("session_id") && w["session_id"].is_string()) {
        ex.session_id = w["session_id"].get<std::string>();
    }
    if (w.contains("task_id") && w["task_id"].is_string()) {
        ex.task_id = w["task_id"].get<std::string>();
    }
    st.execution_context = std::move(ex);
}

void apply_processed_to_agent_state(ProcessedUserInput&& processed, const ExecutionContext& ctx,
                                    internal::AgentThreadState& st) {
    st.initial_user_prompt = std::move(processed.llm_user_text);
    st.pending_injected_context = std::move(processed.injected_context);
    st.pending_control_actions = std::move(processed.control_actions);
    st.pending_input_violations = std::move(processed.tier_a_violations);
    st.execution_context = ctx;
}

std::string take_injected_blocks_as_llm_context(std::vector<InjectedContextBlock>& blocks) {
    std::string out;
    for (auto& b : blocks) {
        std::string ref = b.source_ref;
        if (ref.size() > 200) {
            ref = utf8_safe_truncate(ref, 200);
        }
        out += "\n--- injection:";
        out += b.source_kind;
        out += ":";
        out += ref;
        out += "\n";
        out += b.text_utf8;
    }
    blocks.clear();
    return out;
}

void dispatch_pending_control_actions(std::vector<ControlAction>& actions,
                                      const ExecutionContext* ctx,
                                      internal::AgentThreadState* agent_state,
                                      const AgentConfig* agent_config,
                                      LLMClient* llm_client) {
    for (const auto& a : actions) {
        if (a.command == "memory.clear" && agent_state) {
            apply_memory_clear(*agent_state);
            std::clog << "[user_command] memory.clear applied\n";
        }
    }

    MemoryCompactOptions mcopt;
    mcopt.agent_config = agent_config;
    mcopt.llm_client = llm_client;
    for (const auto& a : actions) {
        if (a.command == "memory.compact") {
            if (agent_state) {
                (void)run_memory_compaction(*agent_state, MemoryCompactTrigger::manual_compact, mcopt);
            }
            std::clog << "[user_command] memory.compact applied\n";
        } else if (a.command == "model.set") {
            std::clog << "[user_command] dispatch model.set args=" << a.args.dump() << " session="
                      << (ctx && ctx->session_id ? *ctx->session_id : std::string{})
                      << " (audit only; does not override LLMClient)\n";
        }
    }
    actions.clear();
}

UserInputPreprocessor::UserInputPreprocessor(PreprocessOptions opt) : opt_(std::move(opt)) {
    if (env_input_tier_b_enabled()) {
        opt_.enable_tier_b = true;
    }
}

ProcessedUserInput UserInputPreprocessor::process(std::string_view raw_user_text,
                                                  const ExecutionContext& ctx) {
    const bool strict = env_input_strict_enabled();
    ProcessedUserInput out;
    int tier_b_calls = 0;
    std::string work = strip_cmd_lines(raw_user_text, strict, ctx, out);

    ContextBudgetLimits blimits = ContextBudgetLimits::load(opt_.agent_config);
    InjectionByteTracker tracker(blimits.max_injection_bytes);

    auto cfg_opt = load_fs_sandbox_config_from_env();

    std::size_t scan = 0;
    std::vector<std::pair<std::size_t, std::size_t>> remove_spans;

    const std::string_view sv(work);
    while (scan < sv.size()) {
        const std::size_t at = sv.find('@', scan);
        if (at == std::string::npos) {
            break;
        }
        const bool file_tok = sv.size() >= at + 6 && sv.compare(at, 6, "@file(") == 0;
        const bool url_tok = sv.size() >= at + 5 && sv.compare(at, 5, "@url(") == 0;
        const bool resource_tok = sv.size() >= at + 2 && sv.compare(at, 2, "@{") == 0;
        if (resource_tok) {
            const std::size_t line_end = sv.find('\n', at);
            const std::size_t close = sv.find('}', at + 2);
            if (close == std::string::npos || (line_end != std::string::npos && close > line_end)) {
                if (strict) out.tier_a_violations.push_back("malformed_resource_uri");
                remove_spans.push_back({at, at + 1});
                scan = at + 1;
                continue;
            }
            const std::string uri_text(sv.substr(at + 2, close - at - 2));
            try {
                const ResourceUri uri = ResourceUri::parse(uri_text);
                if (!ctx.resources) throw std::runtime_error("resource_context_unavailable");
                std::string text;
                if (uri.scheme() == ResourceScheme::Workspace) {
                    if (!opt_.toolbus) throw std::runtime_error("injection_toolbus_unavailable");
                    const auto path = ctx.resources->resolve_local(uri);
                    json fr = opt_.toolbus->call_tool(
                        "fs_read", json{{"path", path.string()}, {"max_bytes", file_inject_max_bytes()}}).get();
                    text = extract_fs_read_text(fr);
                    if (text.empty() && fr.contains("error"))
                        throw std::runtime_error("workspace_resource_fetch_failed:" + tool_err_summary(fr, 160));
                } else if (uri.scheme() == ResourceScheme::Skill) {
                    if (!ctx.skill_loader) throw std::runtime_error("skill_loader_unavailable");
                    const auto& snapshot = ctx.resources->skill_snapshot();
                    const auto entry = snapshot.get(uri.authority());
                    const auto manifest = snapshot.get_manifest(uri.authority());
                    if (!entry || !manifest) throw std::runtime_error("skill_not_found");
                    std::string error;
                    auto loaded = ctx.skill_loader->load_resource_snapshot(
                        *entry, manifest, uri.path(), SkillResourceKind::AnyDeclared,
                        file_inject_max_bytes(), &error);
                    if (!loaded) throw std::runtime_error("skill_resource_fetch_failed:" + error);
                    text = std::move(*loaded);
                } else if (uri.scheme() == ResourceScheme::Mcp) {
                    if (!ctx.resources->mcp_allowed(uri.authority()))
                        throw std::runtime_error("mcp_resource_service_denied");
                    if (!opt_.toolbus) throw std::runtime_error("injection_toolbus_unavailable");
                    const auto contents =
                        opt_.toolbus->read_mcp_resource(uri.authority(), uri.path()).get();
                    if (contents.empty()) throw std::runtime_error("mcp_resource_empty");
                    const std::size_t max_bytes = file_inject_max_bytes();
                    for (const auto& content : contents) {
                        if (content.blob.has_value())
                            throw std::runtime_error("mcp_resource_binary_not_injectable");
                        if (!content.text.has_value())
                            throw std::runtime_error("mcp_resource_content_malformed");
                        const std::size_t separator = text.empty() ? 0U : 1U;
                        if (text.size() > max_bytes || content.text->size() > max_bytes - text.size() ||
                            separator > max_bytes - text.size() - content.text->size())
                            throw std::runtime_error("mcp_resource_too_large");
                        if (separator != 0U) text.push_back('\n');
                        text.append(*content.text);
                    }
                } else {
                    throw std::runtime_error("resource_scheme_not_injectable");
                }
                const std::string header = injection_header("resource", truncate_utf8(uri.str(), 200));
                if (!tracker.try_consume(header, text.size()))
                    throw std::runtime_error("injection_budget_exceeded");
                out.injected_context.push_back(
                    {"resource", uri.str(), std::nullopt, std::move(text), 0});
                out.injected_context.back().byte_length = out.injected_context.back().text_utf8.size();
            } catch (const std::exception& e) {
                out.tier_a_violations.push_back(std::string("resource_injection_failed:") + e.what());
            }
            remove_spans.push_back({at, close + 1});
            scan = close + 1;
            continue;
        }
        if (!file_tok && !url_tok) {
            if (strict) {
                bool resolved = false;
                if (opt_.enable_tier_b && opt_.tier_b_llm &&
                    tier_b_calls < std::max(0, opt_.tier_b_max_calls)) {
                    ++tier_b_calls;
                    const std::size_t frag_len = std::min(sv.size() - at, std::size_t{2048});
                    const auto tier_b = tier_b_try_resolve(
                        sv.substr(at, frag_len), ctx, out, tracker, opt_, cfg_opt);
                    resolved = tier_b == TierBResolveResult::Resolved;
                    if (!resolved && opt_.tier_b_reject_on_failure) {
                        out.tier_a_violations.push_back("tier_b_resolution_failed");
                    }
                }
                if (!resolved) {
                    out.tier_a_violations.push_back("malformed_injection_token");
                }
            }
            remove_spans.push_back({at, at + 1});
            scan = at + 1;
            continue;
        }

        const std::size_t line_end = sv.find('\n', at);
        const std::size_t close = sv.find(')', at);
        if (close == std::string::npos || (line_end != std::string::npos && close > line_end)) {
            if (strict) {
                out.tier_a_violations.push_back("malformed_injection_token");
            } else {
                remove_spans.push_back({at, at + 1});
            }
            scan = at + 1;
            continue;
        }

        const std::size_t inner_beg = file_tok ? at + 6 : at + 5;
        const std::string inner = std::string(sv.substr(inner_beg, close - inner_beg));
        std::string path_or_url;
        std::optional<std::string> range;
        if (file_tok) {
            if (!match_range(inner, path_or_url, range)) {
                if (strict) {
                    out.tier_a_violations.push_back("malformed_injection_token");
                }
                remove_spans.push_back({at, close + 1});
                scan = close + 1;
                continue;
            }
        } else {
            path_or_url = trim_copy(inner);
            if (path_or_url.empty()) {
                if (strict) {
                    out.tier_a_violations.push_back("malformed_injection_token");
                }
                remove_spans.push_back({at, close + 1});
                scan = close + 1;
                continue;
            }
        }

        if (file_tok) {
            if (!opt_.toolbus) {
                out.tier_a_violations.push_back("injection_toolbus_unavailable");
                remove_spans.push_back({at, close + 1});
                scan = close + 1;
                continue;
            }
            if (!cfg_opt) {
                out.tier_a_violations.push_back("file_path_outside_jail");
                remove_spans.push_back({at, close + 1});
                scan = close + 1;
                continue;
            }
            namespace fs = std::filesystem;
            fs::path raw_path(path_or_url);
            fs::path combined = raw_path.is_absolute() ? raw_path : fs::path(ctx.cwd) / raw_path;
            std::error_code ec;
            fs::path canon = fs::weakly_canonical(combined, ec);
            if (ec) {
                out.tier_a_violations.push_back("file_path_outside_jail");
                remove_spans.push_back({at, close + 1});
                scan = close + 1;
                continue;
            }
            json path_err = json::object();
            auto resolved = fs_resolve_under_root(canon.string(), cfg_opt->root, path_err);
            if (!resolved) {
                out.tier_a_violations.push_back("file_path_outside_jail");
                remove_spans.push_back({at, close + 1});
                scan = close + 1;
                continue;
            }

            std::size_t cap_read = cfg_opt->max_read_bytes;
            if (const std::size_t inj_cap = file_inject_max_bytes()) {
                cap_read = std::min(cap_read, inj_cap);
            }
            json fr = opt_.toolbus
                          ->call_tool("fs_read",
                                      json{{"path", resolved->string()}, {"max_bytes", cap_read}})
                          .get();
            std::string text = extract_fs_read_text(fr);
            if (text.empty() && fr.contains("error")) {
                out.tier_a_violations.push_back(
                    std::string("file_fetch_failed:") + tool_err_summary(fr, 200));
                remove_spans.push_back({at, close + 1});
                scan = close + 1;
                continue;
            }
            if (range) {
                text = apply_line_range_str(std::move(text), *range);
            }
            if (text.size() > file_inject_max_bytes()) {
                out.tier_a_violations.push_back("file_inject_too_large");
                remove_spans.push_back({at, close + 1});
                scan = close + 1;
                continue;
            }
            const std::string ref_trunc = truncate_utf8(path_or_url, 200);
            const std::string hdr = injection_header("file", ref_trunc);
            if (!tracker.try_consume(hdr, text.size())) {
                out.tier_a_violations.push_back("injection_budget_exceeded");
                remove_spans.push_back({at, close + 1});
                scan = close + 1;
                continue;
            }
            InjectedContextBlock blk;
            blk.source_kind = "file";
            blk.source_ref = path_or_url;
            blk.text_utf8 = std::move(text);
            blk.byte_length = blk.text_utf8.size();
            out.injected_context.push_back(std::move(blk));
        } else {
            if (!opt_.toolbus) {
                out.tier_a_violations.push_back("injection_toolbus_unavailable");
                remove_spans.push_back({at, close + 1});
                scan = close + 1;
                continue;
            }
            json wr = opt_.toolbus->call_tool("web_fetch", json{{"url", path_or_url}}).get();
            std::string text = extract_web_fetch_text(wr);
            if (text.empty() && wr.contains("error")) {
                out.tier_a_violations.push_back(
                    std::string("url_fetch_failed:") + tool_err_summary(wr, 200));
                remove_spans.push_back({at, close + 1});
                scan = close + 1;
                continue;
            }
            const std::string ref_trunc = truncate_utf8(path_or_url, 200);
            const std::string hdr = injection_header("url", ref_trunc);
            if (!tracker.try_consume(hdr, text.size())) {
                out.tier_a_violations.push_back("injection_budget_exceeded");
                remove_spans.push_back({at, close + 1});
                scan = close + 1;
                continue;
            }
            InjectedContextBlock blk;
            blk.source_kind = "url";
            blk.source_ref = path_or_url;
            blk.text_utf8 = std::move(text);
            blk.byte_length = blk.text_utf8.size();
            out.injected_context.push_back(std::move(blk));
        }

        remove_spans.push_back({at, close + 1});
        scan = close + 1;
    }

    if (!remove_spans.empty()) {
        std::sort(remove_spans.begin(), remove_spans.end());
        std::string rebuilt;
        std::size_t pos = 0;
        for (const auto& sp : remove_spans) {
            if (sp.first > pos) {
                rebuilt.append(sv.substr(pos, sp.first - pos));
            }
            pos = sp.second;
        }
        if (pos < sv.size()) {
            rebuilt.append(sv.substr(pos));
        }
        work = std::move(rebuilt);
    }

    collapse_blank_lines(work);
    out.llm_user_text = trim_copy(work);

    if (!strict && out.llm_user_text.empty() && out.tier_a_violations.empty() &&
        raw_user_text.find_first_not_of(" \t\r\n") != std::string::npos) {
        std::clog << "[input_policy] input_relaxed_degraded policy=" << ctx.input_policy_version << "\n";
    }

    return out;
}

} // namespace agent_framework
