/**
 * @file verifier_types.cpp
 * @brief WP2.8 Verifier parse + gate (phase-2-wp8.md)
 */

#include <agent/agent/verifier_types.hpp>

#include <cctype>
#include <optional>

namespace agent_framework {
namespace {

constexpr std::size_t kDetailMax = 500;
constexpr std::size_t kCodeMax = 64;
constexpr std::size_t kParseErrorSnippet = 256;
constexpr std::size_t kFixTemplateBudget = 2048;
constexpr std::size_t kIssuesJsonBudget = 1536;

bool is_known_action(std::string_view a) {
    return a == "pass" || a == "retry_main" || a == "pass_through" || a == "abort";
}

std::string truncate_utf8_bytes(std::string s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) {
        return s;
    }
    s.resize(max_bytes);
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0U) == 0x80U) {
        s.pop_back();
    }
    return s;
}

std::optional<json> try_strip_markdown_fence(std::string_view raw) {
    std::string_view s = raw;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    if (s.size() >= 7 && s.substr(0, 3) == "```") {
        std::size_t start = 3;
        while (start < s.size() && s[start] != '\n' && s[start] != '\r') {
            ++start;
        }
        if (start < s.size()) {
            ++start;
        }
        std::size_t end = s.rfind("```");
        if (end != std::string_view::npos && end > start) {
            s = s.substr(start, end - start);
        }
    }
    try {
        return json::parse(std::string(s));
    } catch (...) {
        return std::nullopt;
    }
}

VerifierIssue issue_from_json(const json& el) {
    VerifierIssue o;
    if (el.is_object()) {
        o.code = el.value("code", "");
        o.severity = el.value("severity", "block");
        o.detail = el.value("detail", "");
    }
    o.code = truncate_utf8_bytes(std::move(o.code), kCodeMax);
    if (o.severity != "info" && o.severity != "warn" && o.severity != "block") {
        o.severity = "block";
    }
    o.detail = truncate_utf8_bytes(std::move(o.detail), kDetailMax);
    if (o.code.empty()) {
        o.code = "unspecified";
    }
    if (o.detail.empty()) {
        o.detail = "(no detail)";
    }
    return o;
}

VerifierResult synthetic_parse_error(std::string_view raw) {
    std::string snippet;
    snippet.assign(raw.data(), raw.size());
    snippet = truncate_utf8_bytes(std::move(snippet), kParseErrorSnippet);
    VerifierResult r;
    r.ok = false;
    VerifierIssue i;
    i.code = "verifier_parse_error";
    i.severity = "block";
    i.detail = std::move(snippet);
    r.issues.push_back(std::move(i));
    r.suggested_action = "pass_through";
    return r;
}

} // namespace

VerifierResult make_verifier_timeout_result(std::string_view detail) {
    VerifierResult r;
    r.ok = false;
    VerifierIssue i;
    i.code = "verifier_timeout";
    i.severity = "block";
    i.detail = detail.empty() ? "verifier LLM timeout" : std::string(detail);
    i.detail = truncate_utf8_bytes(std::move(i.detail), kDetailMax);
    r.issues.push_back(std::move(i));
    r.suggested_action = "pass_through";
    return r;
}

VerifierResult parse_verifier_response(std::string_view raw,
                                      int verifier_retry_count,
                                      int max_retries) {
    std::optional<json> j = try_strip_markdown_fence(raw);
    if (!j || !j->is_object()) {
        return synthetic_parse_error(raw);
    }

    const json& o = *j;
    VerifierResult r;

    if (o.contains("issues") && o["issues"].is_array()) {
        for (const auto& el : o["issues"]) {
            r.issues.push_back(issue_from_json(el));
        }
    } else if (o.contains("issues")) {
        return synthetic_parse_error(raw);
    }

    if (o.contains("ok") && o["ok"].is_boolean()) {
        r.ok = o["ok"].get<bool>();
    } else if (o.contains("ok")) {
        return synthetic_parse_error(raw);
    } else {
        r.ok = false;
    }

    if (o.contains("suggested_action") && o["suggested_action"].is_string()) {
        r.suggested_action = o["suggested_action"].get<std::string>();
    } else {
        if (r.ok) {
            r.suggested_action = "pass";
        } else {
            if (verifier_retry_count < max_retries) {
                r.suggested_action = "retry_main";
            } else {
                r.suggested_action = "pass_through";
            }
        }
    }

    return r;
}

VerifierGateOutcome apply_verifier_gate(const VerifierResult& r,
                                       int verifier_retry_count,
                                       int max_retries) {
    VerifierGateOutcome out;
    out.action_unknown_normalized = false;

    // §5.1 step 1: abort first, exact token (before unknown normalization).
    if (r.suggested_action == "abort") {
        out.kind = VerifierGateKind::Abort;
        out.verifier_ok = false;
        out.effective_action = "abort";
        return out;
    }

    std::string action = r.suggested_action;
    if (!is_known_action(action)) {
        action = "pass_through";
        out.action_unknown_normalized = true;
    }

    if (r.ok) {
        out.kind = VerifierGateKind::PublishPass;
        out.verifier_ok = true;
        out.effective_action = "pass";
        return out;
    }

    if (action == "retry_main" && verifier_retry_count < max_retries) {
        out.kind = VerifierGateKind::RetryMain;
        out.verifier_ok = false;
        out.effective_action = "retry_main";
        return out;
    }

    if (action == "retry_main" && verifier_retry_count >= max_retries) {
        out.kind = VerifierGateKind::PublishPassThrough;
        out.verifier_ok = false;
        out.effective_action = "pass_through";
        return out;
    }

    out.kind = VerifierGateKind::PublishPassThrough;
    out.verifier_ok = false;
    out.effective_action = "pass_through";
    return out;
}

std::string build_verifier_fix_system_message(const std::vector<VerifierIssue>& issues) {
    json arr = json::array();
    for (const auto& i : issues) {
        json one;
        one["code"] = i.code;
        one["severity"] = i.severity;
        one["detail"] = i.detail;
        arr.push_back(std::move(one));
    }
    std::string compact = arr.dump();
    compact = truncate_utf8_bytes(std::move(compact), kIssuesJsonBudget);

    const char* prefix =
        "The previous draft answer did not pass verification. Address the following issues "
        "and produce a corrected final answer without repeating the verification "
        "instructions.\nIssues (JSON): ";
    std::string msg = prefix + compact;
    msg = truncate_utf8_bytes(std::move(msg), kFixTemplateBudget);
    return msg;
}

} // namespace agent_framework
