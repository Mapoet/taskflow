/**
 * @file verifier_runner.cpp
 * @brief WP2.8 Verifier runner (phase-2-wp8.md)
 */

#include <agent/internal/agent_thread_state.hpp>
#include <agent/agent/verifier_runner.hpp>

#include <agent/prompt_renderer/prompt_renderer.hpp>

#include <cctype>
#include <cstdlib>
#include <future>
#include <sstream>

namespace agent_framework {
namespace {

bool env_truthy(const char* key) {
    const char* v = std::getenv(key);
    if (!v || !*v) {
        return false;
    }
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T';
}

std::string truncate_utf8(std::string s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) {
        return s;
    }
    s.resize(max_bytes);
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0U) == 0x80U) {
        s.pop_back();
    }
    return s;
}

std::string summarize_message(const Message& m, std::size_t cap) {
    std::ostringstream oss;
    oss << m.role << ": ";
    if (m.role == "tool" && m.tool_name) {
        oss << *m.tool_name << " ";
    }
    std::string body = m.content;
    if (m.tool_result) {
        body = m.tool_result->dump();
    }
    body = truncate_utf8(std::move(body), cap);
    oss << body;
    return oss.str();
}

} // namespace

const char* verifier_system_prompt() {
    static const char* k =
        "You are a strict output verifier. Given the user task and a draft assistant answer, "
        "respond with exactly one JSON object and no markdown fences. "
        "Keys: ok (boolean), issues (array of {code, severity, detail}), "
        "suggested_action (string: pass | retry_main | pass_through | abort). "
        "issues must be [] if ok is true. "
        "Do not suggest calling tools or external APIs.";
    return k;
}

std::shared_ptr<LLMClient> make_verifier_llm_client_from_env() {
    auto client = std::make_shared<LLMClient>(LLMClient::from_env());
    client->set_prompt_renderer(std::make_shared<PromptRenderer>());
    const char* pe = std::getenv("AGENT_LLM_PROVIDER");
    std::string prov = pe ? std::string(pe) : "openai";
    ModelConfig mc;
    if (const char* t = std::getenv("AGENT_HTTP_TIMEOUT_SEC")) {
        const int v = std::atoi(t);
        if (v > 0) {
            mc.http_timeout_sec = v;
        }
    }
    if (const char* r = std::getenv("AGENT_LLM_MAX_RETRIES")) {
        const int v = std::atoi(r);
        if (v >= 0) {
            mc.max_retries = v;
        }
    }
    if (const char* vm = std::getenv("AGENT_VERIFIER_MODEL"); vm && vm[0] != '\0') {
        mc.model_name = vm;
    } else {
        mc.model_name = client->get_model_name(prov);
    }
    if (const char* tv = std::getenv("AGENT_VERIFIER_TEMPERATURE")) {
        mc.temperature = std::strtod(tv, nullptr);
    } else {
        mc.temperature = 0.0;
    }
    client->configure(prov, mc);
    return client;
}

std::size_t verifier_prompt_max_bytes_from_env() {
    const char* e = std::getenv("AGENT_VERIFIER_PROMPT_MAX_BYTES");
    if (!e || !*e) {
        return 65536;
    }
    const auto v = static_cast<std::size_t>(std::strtoull(e, nullptr, 10));
    return v ? v : 65536;
}

int verifier_timeout_ms_from_env() {
    const char* e = std::getenv("AGENT_VERIFIER_TIMEOUT_MS");
    if (!e || !*e) {
        return 30000;
    }
    const int v = std::atoi(e);
    return v > 0 ? v : 30000;
}

int verifier_max_retries_from_env() {
    const char* e = std::getenv("AGENT_VERIFIER_MAX_RETRIES");
    if (!e || !*e) {
        return 1;
    }
    const int v = std::atoi(e);
    return v < 0 ? 1 : v;
}

bool verifier_include_tool_trace_from_env() {
    return env_truthy("AGENT_VERIFIER_INCLUDE_TOOL_TRACE");
}

bool verifier_redact_ids_from_env() {
    return env_truthy("AGENT_VERIFIER_REDACT_IDS");
}

int verifier_history_turns_from_env() {
    const char* e = std::getenv("AGENT_VERIFIER_HISTORY_TURNS");
    if (!e || !*e) {
        return 3;
    }
    const int v = std::atoi(e);
    return v > 0 ? v : 3;
}

std::string build_verifier_user_json(const internal::AgentThreadState& state,
                                    std::string_view draft_final_answer,
                                    std::size_t max_bytes) {
    json j;
    std::string user_query = state.initial_user_prompt;
    if (user_query.empty()) {
        for (const auto& m : state.history) {
            if (m.role == "user" && !m.content.empty()) {
                user_query = m.content;
                break;
            }
        }
    }
    j["user_query"] = user_query;
    j["draft_final_answer"] = std::string(draft_final_answer);

    const int n_turns = verifier_history_turns_from_env();
    json digest_rev = json::array();
    int assistant_blocks = 0;
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(state.history.size()) - 1;
         i >= 0 && assistant_blocks < n_turns; --i) {
        const Message& m = state.history[static_cast<std::size_t>(i)];
        if (m.role == "assistant") {
            ++assistant_blocks;
        }
        if (m.role == "assistant" || m.role == "tool") {
            digest_rev.push_back(summarize_message(m, 512));
        }
    }
    json digest = json::array();
    for (auto it = digest_rev.rbegin(); it != digest_rev.rend(); ++it) {
        digest.push_back(*it);
    }
    j["history_digest"] = std::move(digest);

    if (verifier_include_tool_trace_from_env()) {
        json trace = json::array();
        for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(state.history.size()) - 1;
             i >= 0 && trace.size() < 16; --i) {
            const Message& m = state.history[static_cast<std::size_t>(i)];
            if (m.role != "tool" || !m.tool_name) {
                continue;
            }
            json one;
            one["tool_name"] = *m.tool_name;
            std::string excerpt;
            if (m.tool_result) {
                excerpt = m.tool_result->dump();
            } else {
                excerpt = m.content;
            }
            one["result_excerpt"] = truncate_utf8(std::move(excerpt), 512);
            trace.push_back(std::move(one));
        }
        j["tool_trace"] = std::move(trace);
    }

    if (!verifier_redact_ids_from_env() && state.execution_context) {
        const ExecutionContext& cx = *state.execution_context;
        if (cx.session_id) {
            j["session_id"] = *cx.session_id;
        }
        if (cx.task_id) {
            j["task_id"] = *cx.task_id;
        }
    }

    const std::string trailer = "\n[verifier_input_truncated]\n";
    std::string out = j.dump();
    if (out.size() <= max_bytes) {
        return out;
    }
    json j2 = j;
    std::string draft = j2["draft_final_answer"].get<std::string>();
    while (out.size() > max_bytes && draft.size() > 64) {
        draft.resize(draft.size() * 3 / 4);
        draft = truncate_utf8(std::move(draft), draft.size());
        j2["draft_final_answer"] = draft + trailer;
        out = j2.dump();
    }
    if (out.size() > max_bytes) {
        j2["draft_final_answer"] = std::string("[truncated]") + trailer;
        j2["history_digest"] = json::array();
        j2.erase("tool_trace");
        out = j2.dump();
        out = truncate_utf8(std::move(out), max_bytes);
    }
    return out;
}

VerifierRunOutput run_verifier_sync(LLMClient& llm,
                                   std::string_view verifier_user_json,
                                   int verifier_retry_count,
                                   int max_retries,
                                   int timeout_ms) {
    VerifierRunOutput out;
    LLMInput in;
    in.system_prompt = verifier_system_prompt();
    in.user_prompt.assign(verifier_user_json.data(), verifier_user_json.size());
    in.tools.clear();
    in.history.clear();
    in.context.clear();

    const auto t0 = std::chrono::steady_clock::now();
    std::future<LLMOutput> fut = llm.invoke(in, "", nullptr);
    std::future_status st = std::future_status::deferred;
    if (timeout_ms > 0) {
        st = fut.wait_for(std::chrono::milliseconds(timeout_ms));
    } else {
        st = fut.wait_for(std::chrono::hours(24));
    }
    const auto t1 = std::chrono::steady_clock::now();
    out.latency_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

    // deferred：wait_for 可能立即返回 deferred 且不执行；须 get() 才会跑 std::launch::deferred 任务。
    // timeout：共享状态在时限内未就绪。
    if (timeout_ms > 0 && st == std::future_status::timeout) {
        out.parsed = make_verifier_timeout_result("verifier LLM wait timed out");
        return out;
    }

    try {
        LLMOutput lo = fut.get();
        std::string raw = lo.final_answer.empty() ? lo.reasoning : lo.final_answer;
        out.parsed = parse_verifier_response(raw, verifier_retry_count, max_retries);
    } catch (...) {
        VerifierResult er;
        er.ok = false;
        VerifierIssue is;
        is.code = "verifier_invoke_error";
        is.severity = "block";
        is.detail = "verifier LLM invoke threw";
        er.issues.push_back(std::move(is));
        er.suggested_action = "pass_through";
        out.parsed = std::move(er);
    }
    return out;
}

} // namespace agent_framework
