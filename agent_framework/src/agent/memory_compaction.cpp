/**
 * @file memory_compaction.cpp
 * @brief WP2.9 memory compaction / auto trigger
 */

#include <agent/context_budget/context_budget.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/agent/memory_compaction.hpp>
#include <agent/skills/skill_supply_chain.hpp>

#include <agent/core/types.hpp>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <future>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace agent_framework {
namespace {

bool env_truthy_default_on(const char* key) {
    const char* v = std::getenv(key);
    if (v == nullptr) {
        return true;
    }
    if (v[0] == '\0') {
        return true;
    }
    std::string s(v);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return !(s == "0" || s == "false" || s == "no" || s == "off");
}

int memory_auto_min_steps_from_env() {
    const char* e = std::getenv("AGENT_MEMORY_AUTO_MIN_STEPS");
    if (!e || !*e) {
        return 1;
    }
    const int n = std::atoi(e);
    return n <= 0 ? 1 : n;
}

std::size_t memory_head_keep_from_env() {
    const char* e = std::getenv("AGENT_MEMORY_COMPACT_HEAD_KEEP");
    if (!e || !*e) {
        return 1;
    }
    const auto v = static_cast<std::size_t>(std::strtoull(e, nullptr, 10));
    return v;
}

std::size_t memory_tail_keep_from_env() {
    const char* e = std::getenv("AGENT_MEMORY_COMPACT_TAIL_KEEP");
    if (!e || !*e) {
        return 8;
    }
    const auto v = static_cast<std::size_t>(std::strtoull(e, nullptr, 10));
    return v;
}

std::size_t memory_soft_limit_from_env() {
    const char* e = std::getenv("AGENT_MEMORY_SOFT_LIMIT_BYTES");
    if (!e || !*e) {
        return 1048576;
    }
    return static_cast<std::size_t>(std::strtoull(e, nullptr, 10));
}

std::size_t memory_hard_limit_from_env() {
    const char* e = std::getenv("AGENT_MEMORY_HARD_LIMIT_BYTES");
    if (!e || !*e) {
        return 2097152;
    }
    return static_cast<std::size_t>(std::strtoull(e, nullptr, 10));
}

double memory_trigger_ratio_from_env() {
    const char* e = std::getenv("AGENT_MEMORY_COMPACT_TRIGGER_RATIO");
    if (!e || !*e) {
        return 0.5;
    }
    const double r = std::strtod(e, nullptr);
    if (r < 0.0) {
        return 0.5;
    }
    if (r > 1.0) {
        return 1.0;
    }
    return r;
}

int memory_summary_timeout_ms_from_env() {
    const char* e = std::getenv("AGENT_MEMORY_SUMMARY_TIMEOUT_MS");
    if (!e || !*e) {
        return 20000;
    }
    const int v = std::atoi(e);
    return v > 0 ? v : 20000;
}

std::size_t memory_summary_max_out_from_env() {
    const char* e = std::getenv("AGENT_MEMORY_SUMMARY_MAX_OUT_BYTES");
    if (!e || !*e) {
        return 4096;
    }
    const auto v = static_cast<std::size_t>(std::strtoull(e, nullptr, 10));
    return v ? v : 4096;
}

std::size_t memory_summary_input_max_from_env() {
    const char* e = std::getenv("AGENT_MEMORY_SUMMARY_INPUT_MAX_BYTES");
    if (!e || !*e) {
        return 80000;
    }
    const auto v = static_cast<std::size_t>(std::strtoull(e, nullptr, 10));
    return v ? v : 80000;
}

bool memory_compact_mode_summarize() {
    const char* e = std::getenv("AGENT_MEMORY_COMPACT_MODE");
    if (!e || !*e) {
        return false;
    }
    std::string s(e);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s == "summarize";
}

std::string memory_compactor_name() {
    const char* e = std::getenv("AGENT_MEMORY_COMPACTOR");
    if (e && *e) return e;
    return memory_compact_mode_summarize() ? "structured" : "truncate";
}

std::string trigger_wp29_string(MemoryCompactTrigger why) {
    switch (why) {
        case MemoryCompactTrigger::auto_threshold:
            return "auto";
        case MemoryCompactTrigger::manual_compact:
            return "manual";
        case MemoryCompactTrigger::hard_cap:
            return "hard";
    }
    return "auto";
}

std::string make_truncate_marker(MemoryCompactTrigger why,
                                std::size_t dropped,
                                std::size_t b0,
                                std::size_t b1) {
    std::ostringstream oss;
    oss << "[memory_compacted mode=truncate trigger=" << trigger_wp29_string(why)
        << " dropped_messages=" << dropped << " bytes_before=" << b0 << " bytes_after=" << b1
        << " policy=wp29-v1]";
    std::string s = oss.str();
    if (s.size() > 512) {
        s = utf8_safe_truncate(s, 512);
    }
    return s;
}

bool do_truncate_middle(internal::AgentThreadState& st,
                       MemoryCompactTrigger why,
                       std::size_t H_env,
                       std::size_t T_env,
                       MemoryCompactResult& out,
                       std::size_t bytes_before) {
    const std::size_t n = st.history.size();
    if (n == 0) {
        out.bytes_before = bytes_before;
        out.bytes_after = bytes_before;
        out.did_mutate = false;
        out.outcome = MemoryCompactionOutcome::NoOp;
        out.strategy_used = "truncate";
        out.log_reason = "empty_history";
        return false;
    }
    std::size_t H = std::min(std::max(H_env, std::size_t{1}), n);
    std::size_t T = std::min(T_env, n);
    if (H + T >= n) {
        std::clog << "[memory] compact noop reason=min_size\n";
        out.bytes_before = bytes_before;
        out.bytes_after = bytes_before;
        out.did_mutate = false;
        out.outcome = MemoryCompactionOutcome::NoOp;
        out.strategy_used = "truncate";
        out.log_reason = "min_size";
        return false;
    }
    std::vector<Message> head(st.history.begin(), st.history.begin() + static_cast<std::ptrdiff_t>(H));
    std::vector<Message> tail(st.history.end() - static_cast<std::ptrdiff_t>(T), st.history.end());
    const std::size_t dropped = n - H - T;
    st.history = std::move(head);
    Message sys;
    sys.role = "system";
    sys.timestamp = std::time(nullptr);
    const std::size_t b0 = bytes_before;
    st.history.push_back(sys);
    const std::size_t mid_idx = st.history.size() - 1;
    for (auto& m : tail) {
        st.history.push_back(std::move(m));
    }
    const std::size_t b1_intermediate = history_utf8_bytes_total(st);
    st.history[mid_idx].content =
        make_truncate_marker(why, dropped, b0, b1_intermediate);
    const std::size_t b1 = history_utf8_bytes_total(st);
    out.did_mutate = true;
    out.outcome = MemoryCompactionOutcome::Mutated;
    out.strategy_used = "truncate";
    out.bytes_before = b0;
    out.bytes_after = b1;
    out.log_reason = "truncate_ok";
    return true;
}

std::string linearize_middle(const std::vector<Message>& hist,
                            std::size_t H,
                            std::size_t n,
                            std::size_t T) {
    std::ostringstream oss;
    for (std::size_t i = H; i + T < n; ++i) {
        const Message& m = hist[i];
        oss << m.role << ": ";
        if (m.role == "tool" && m.tool_result) {
            try {
                oss << json_compact_dump(*m.tool_result);
            } catch (...) {
                oss << "<tool_result>";
            }
        } else {
            oss << m.content;
        }
        oss << "\n---\n";
    }
    return oss.str();
}

enum class StructuredAttempt { Success, Failure, Cancelled };

StructuredAttempt try_summarize_middle(internal::AgentThreadState& st,
                         MemoryCompactTrigger why,
                         std::size_t H_env,
                         std::size_t T_env,
                         const MemoryCompactionContext& context,
                         MemoryCompactResult& out,
                         std::size_t bytes_before) {
    const std::size_t n = st.history.size();
    std::size_t H = std::min(std::max(H_env, std::size_t{1}), n);
    std::size_t T = std::min(T_env, n);
    if (H + T >= n) {
        return StructuredAttempt::Failure;
    }
    std::string middle =
        linearize_middle(st.history, H, n, T);
    const std::string digest = skill_sha256_bytes(middle).value_or("sha256-unavailable");
    const std::size_t mid_cap = context.profile.max_input_bytes
        ? context.profile.max_input_bytes : memory_summary_input_max_from_env();
    middle = utf8_safe_truncate(middle, mid_cap);

    LLMInput lin;
    lin.system_prompt =
        "You compress a middle segment of an agent conversation. Output exactly one JSON object "
        "with schema_version=1, summary(string), facts(array), open_items(array), and "
        "source_digest(string). Copy the supplied source_digest exactly. No markdown fences.";
    lin.user_prompt = "source_digest=" + digest + "\n\n" + middle;
    lin.tools.clear();
    lin.history.clear();
    lin.cancellation_requested = context.cancellation_requested;

    LLMOutput lo;
    bool received = false;
    const int attempts = std::max(1, context.profile.max_retries + 1);
    for(int attempt = 0; attempt < attempts && !received; ++attempt) {
        if(context.cancellation_requested && context.cancellation_requested()) {
            out.cancelled = true;
            out.outcome = MemoryCompactionOutcome::Cancelled;
            out.log_reason = "structured_cancelled";
            return StructuredAttempt::Cancelled;
        }
        try {
            auto future = context.sub_llm->invoke(lin, context.profile.provider, nullptr);
            const auto timeout = std::chrono::milliseconds(
                context.profile.timeout_ms > 0 ? context.profile.timeout_ms
                                               : memory_summary_timeout_ms_from_env());
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            for(;;) {
                if(context.cancellation_requested && context.cancellation_requested()) {
                    out.cancelled = true;
                    out.outcome = MemoryCompactionOutcome::Cancelled;
                    out.log_reason = "structured_cancelled";
                    return StructuredAttempt::Cancelled;
                }
                const auto status = future.wait_for(std::chrono::milliseconds(10));
                if(status == std::future_status::ready || status == std::future_status::deferred) {
                    lo = future.get();
                    received = true;
                    break;
                }
                if(std::chrono::steady_clock::now() >= deadline) break;
            }
        } catch (...) {
            received = false;
        }
    }
    if(!received) return StructuredAttempt::Failure;
    std::string raw = lo.final_answer.empty() ? lo.reasoning : lo.final_answer;
    json j;
    try {
        j = json::parse(raw);
    } catch (...) {
        return StructuredAttempt::Failure;
    }
    if (!j.is_object() || j.value("schema_version", 0) != 1 ||
        !j.contains("summary") || !j["summary"].is_string() ||
        !j.contains("facts") || !j["facts"].is_array() ||
        !j.contains("open_items") || !j["open_items"].is_array() ||
        j.value("source_digest", "") != digest) {
        return StructuredAttempt::Failure;
    }
    const auto max_output = context.profile.max_output_bytes
        ? context.profile.max_output_bytes : memory_summary_max_out_from_env();
    const std::string summary = j.dump();
    if(summary.size() > max_output) return StructuredAttempt::Failure;

    std::vector<Message> head(st.history.begin(), st.history.begin() + static_cast<std::ptrdiff_t>(H));
    std::vector<Message> tail(st.history.end() - static_cast<std::ptrdiff_t>(T), st.history.end());
    const std::size_t dropped = n - H - T;
    st.history = std::move(head);
    Message sys;
    sys.role = "system";
    sys.timestamp = std::time(nullptr);
    const std::size_t b0 = bytes_before;
    st.history.push_back(sys);
    const std::size_t mid_idx = st.history.size() - 1;
    for (auto& m : tail) {
        st.history.push_back(std::move(m));
    }
    st.history[mid_idx].content =
        std::string("[memory_compacted mode=structured trigger=") + trigger_wp29_string(why) +
        " dropped_messages=" + std::to_string(dropped) + " bytes_before=" + std::to_string(b0) +
        " schema_version=1 source_digest=" + digest + " policy=wp33-v1]\n" +
        summary;
    if (st.history[mid_idx].content.size() > 512 + max_output) {
        st.history[mid_idx].content =
            utf8_safe_truncate(st.history[mid_idx].content,
                              512 + max_output);
    }
    const std::size_t b1 = history_utf8_bytes_total(st);
    out.did_mutate = true;
    out.outcome = MemoryCompactionOutcome::Mutated;
    out.strategy_used = "structured";
    out.bytes_before = b0;
    out.bytes_after = b1;
    out.log_reason = "summarize_ok";
    out.source_digest = digest;
    out.schema_version = 1;
    return StructuredAttempt::Success;
}

bool try_extractive_middle(internal::AgentThreadState& st, MemoryCompactTrigger why,
                           std::size_t H_env, std::size_t T_env,
                           MemoryCompactResult& out, std::size_t bytes_before) {
    const std::size_t n = st.history.size();
    const std::size_t H = std::min(std::max(H_env, std::size_t{1}), n);
    const std::size_t T = std::min(T_env, n);
    if (H + T >= n) return false;
    const auto middle = linearize_middle(st.history, H, n, T);
    std::vector<Message> head(st.history.begin(), st.history.begin() + static_cast<std::ptrdiff_t>(H));
    std::vector<Message> tail(st.history.end() - static_cast<std::ptrdiff_t>(T), st.history.end());
    st.history = std::move(head);
    Message summary; summary.role = "system"; summary.timestamp = std::time(nullptr);
    summary.content = "[memory_compacted mode=extractive trigger=" + trigger_wp29_string(why) + "]\n" +
        utf8_safe_truncate(middle, memory_summary_max_out_from_env());
    st.history.push_back(std::move(summary));
    for (auto& m : tail) st.history.push_back(std::move(m));
    out.did_mutate = true; out.outcome = MemoryCompactionOutcome::Mutated;
    out.strategy_used = "extractive"; out.bytes_before = bytes_before;
    out.bytes_after = history_utf8_bytes_total(st); out.log_reason = "extractive_ok";
    return true;
}

void enforce_hard_limit_history(internal::AgentThreadState& st,
                               std::size_t hard_bytes,
                               const ContextBudgetLimits& limits,
                               bool& logged) {
    if (hard_bytes == 0) {
        return;
    }
    for (;;) {
        const std::size_t bytes = history_utf8_bytes_total(st);
        if (bytes <= hard_bytes) {
            break;
        }
        bool progressed = false;
        for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(st.history.size()) - 1; i >= 0; --i) {
            Message& m = st.history[static_cast<std::size_t>(i)];
            if (m.role == "tool" && m.tool_result) {
                std::string w;
                (void)apply_per_tool_result_budget(*m.tool_result, limits, AfTruncationKind::tool_result,
                                                  &w);
                logged = true;
                progressed = true;
                break;
            }
        }
        if (progressed) {
            continue;
        }
        for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(st.history.size()) - 1; i >= 0; --i) {
            Message& m = st.history[static_cast<std::size_t>(i)];
            if ((m.role == "assistant" || m.role == "user") && m.content.size() > 64) {
                const std::size_t nl = std::max<std::size_t>(64, m.content.size() * 3 / 4);
                m.content = utf8_safe_truncate(m.content, nl);
                logged = true;
                progressed = true;
                break;
            }
        }
        if (!progressed) {
            break;
        }
    }
}

std::string history_digest(const internal::AgentThreadState& state) {
    std::ostringstream canonical;
    for(const auto& message : state.history) {
        canonical << message.role.size() << ':' << message.role << '|'
                  << message.content.size() << ':' << message.content << '|';
        if(message.tool_result) canonical << message.tool_result->dump();
        canonical << '\n';
    }
    return skill_sha256_bytes(canonical.str()).value_or("sha256-unavailable");
}

class TruncateCompactor final : public MemoryCompactor {
public:
    MemoryCompactResult compact(const MemoryCompactionInput& input,
                                const MemoryCompactionContext&) override {
        MemoryCompactResult result;
        result.source_digest = history_digest(input.state);
        (void)do_truncate_middle(input.state, input.trigger, input.head_keep,
                                 input.tail_keep, result, input.bytes_before);
        return result;
    }
};

class ExtractiveCompactor final : public MemoryCompactor {
public:
    MemoryCompactResult compact(const MemoryCompactionInput& input,
                                const MemoryCompactionContext&) override {
        MemoryCompactResult result;
        result.source_digest = history_digest(input.state);
        (void)try_extractive_middle(input.state, input.trigger, input.head_keep,
                                    input.tail_keep, result, input.bytes_before);
        return result;
    }
};

class StructuredSummaryCompactor final : public MemoryCompactor {
public:
    MemoryCompactResult compact(const MemoryCompactionInput& input,
                                const MemoryCompactionContext& context) override {
        MemoryCompactResult result;
        result.bytes_before = input.bytes_before;
        result.bytes_after = input.bytes_before;
        result.strategy_used = "structured";
        result.source_digest = history_digest(input.state);
        if(!context.sub_llm) {
            result.outcome = MemoryCompactionOutcome::Failed;
            result.log_reason = "structured_sub_llm_unavailable";
            return result;
        }
        const auto outcome = try_summarize_middle(
            input.state, input.trigger, input.head_keep, input.tail_keep,
            context, result, input.bytes_before);
        if(outcome == StructuredAttempt::Cancelled) {
            result.cancelled = true;
            result.outcome = MemoryCompactionOutcome::Cancelled;
        } else if(outcome == StructuredAttempt::Failure) {
            result.outcome = MemoryCompactionOutcome::Failed;
            result.log_reason = "structured_failed";
        }
        return result;
    }
};

} // namespace

void MemoryCompactorRegistry::register_compactor(
    std::string id, std::shared_ptr<MemoryCompactor> compactor) {
    if(id.empty() || !compactor) throw std::invalid_argument("memory compactor id/implementation required");
    std::lock_guard<std::mutex> lock(mutex_);
    if(!compactors_.emplace(std::move(id), std::move(compactor)).second)
        throw std::invalid_argument("memory compactor already registered");
}

std::shared_ptr<MemoryCompactor> MemoryCompactorRegistry::resolve(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = compactors_.find(std::string(id));
    return found == compactors_.end() ? nullptr : found->second;
}

std::vector<std::string> MemoryCompactorRegistry::ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    result.reserve(compactors_.size());
    for(const auto& [id, _] : compactors_) result.push_back(id);
    return result;
}

std::shared_ptr<MemoryCompactorRegistry> default_memory_compactor_registry() {
    static auto registry = [] {
        auto value = std::make_shared<MemoryCompactorRegistry>();
        value->register_compactor("truncate", std::make_shared<TruncateCompactor>());
        value->register_compactor("extractive", std::make_shared<ExtractiveCompactor>());
        value->register_compactor("structured", std::make_shared<StructuredSummaryCompactor>());
        return value;
    }();
    return registry;
}

void apply_memory_clear(internal::AgentThreadState& st) {
    st.history.clear();
    st.last_error.clear();
    st.last_memory_auto_compact_iteration = -1;
    st.iteration = 0;
    st.skill_prompt_cache.reset();
    st.active_skill_id.reset();
    st.verifier_retry_count = 0;
}

MemoryCompactResult run_memory_compaction(internal::AgentThreadState& st,
                                         MemoryCompactTrigger why,
                                         const MemoryCompactOptions& opt) {
    static thread_local bool busy = false;
    MemoryCompactResult res;
    const std::size_t bytes_before = history_utf8_bytes_total(st);
    res.bytes_before = bytes_before;
    if (busy) {
        res.bytes_after = bytes_before;
        res.log_reason = "reentrant_skip";
        return res;
    }
    struct Bus {
        bool& b;
        explicit Bus(bool& x) : b(x) {
            b = true;
        }
        ~Bus() {
            b = false;
        }
    } guard(busy);

    const std::size_t H_env = memory_head_keep_from_env();
    const std::size_t T_env = memory_tail_keep_from_env();
    const ContextBudgetLimits limits = ContextBudgetLimits::load(opt.agent_config);
    const std::string selected = opt.strategy_id.empty() ? memory_compactor_name()
                                                          : opt.strategy_id;
    const auto registry = opt.registry ? opt.registry : default_memory_compactor_registry();
    auto compactor = registry->resolve(selected);
    if(!compactor) throw std::invalid_argument("unknown memory compactor: " + selected);
    MemoryCompactionProfile profile = opt.profile;
    if(profile.timeout_ms <= 0) profile.timeout_ms = memory_summary_timeout_ms_from_env();
    if(profile.max_input_bytes == 0) profile.max_input_bytes = memory_summary_input_max_from_env();
    if(profile.max_output_bytes == 0) profile.max_output_bytes = memory_summary_max_out_from_env();
    MemoryCompactionContext context{opt.agent_config,
        opt.sub_llm_client ? opt.sub_llm_client : opt.llm_client,
        profile, opt.cancellation_requested};
    MemoryCompactionInput input{st, why, H_env, T_env, bytes_before};
    res = compactor->compact(input, context);
    if(res.outcome == MemoryCompactionOutcome::Cancelled || res.cancelled) {
        res.outcome = MemoryCompactionOutcome::Cancelled;
        res.cancelled = true;
        res.bytes_after = history_utf8_bytes_total(st);
        return res;
    }
    const auto builtins = default_memory_compactor_registry();
    const auto fallback = [&](std::string_view id) {
        auto resolved = registry->resolve(id);
        return resolved ? resolved : builtins->resolve(id);
    };
    if(res.outcome == MemoryCompactionOutcome::Failed && selected == "structured") {
        const auto extractive = fallback("extractive");
        if(!extractive) throw std::logic_error("built-in extractive compactor unavailable");
        res = extractive->compact(input, context);
        if(res.outcome == MemoryCompactionOutcome::Mutated) {
            res.strategy_used = "fallback_extractive";
            res.log_reason = "structured_failed";
        }
    }
    if(res.outcome == MemoryCompactionOutcome::Failed && selected != "truncate") {
        const auto truncate = fallback("truncate");
        if(!truncate) throw std::logic_error("built-in truncate compactor unavailable");
        res = truncate->compact(input, context);
        if(res.outcome == MemoryCompactionOutcome::Mutated) {
            res.strategy_used = "fallback_truncate";
            res.log_reason = selected + "_failed";
        }
    }

    bool logged_fallback = false;
    const std::size_t hard = memory_hard_limit_from_env();
    enforce_hard_limit_history(st, hard, limits, logged_fallback);
    res.bytes_after = history_utf8_bytes_total(st);
    res.did_mutate = res.bytes_after != bytes_before;
    if(res.did_mutate) res.outcome = MemoryCompactionOutcome::Mutated;
    if (logged_fallback) {
        std::clog << "[memory] fallback_1c bytes=" << res.bytes_after << "\n";
    }
    if (res.did_mutate) {
        st.last_memory_compaction_ts = std::time(nullptr);
        std::clog << "[memory] compact strategy=" << res.strategy_used << " bytes "
                  << res.bytes_before << "->" << res.bytes_after << " trigger="
                  << trigger_wp29_string(why) << "\n";
    }
    return res;
}

void maybe_auto_compact_memory(internal::AgentThreadState& st, const MemoryCompactOptions& opt) {
    static thread_local bool in_maybe = false;
    if (in_maybe) {
        return;
    }
    struct Guard {
        bool& b;
        explicit Guard(bool& x) : b(x) {
            b = true;
        }
        ~Guard() {
            b = false;
        }
    } g(in_maybe);

    if (!env_truthy_default_on("AGENT_MEMORY_AUTO_COMPACT")) {
        return;
    }

    const std::size_t soft = memory_soft_limit_from_env();
    const std::size_t hard = memory_hard_limit_from_env();
    const double ratio = memory_trigger_ratio_from_env();
    const std::size_t bytes = history_utf8_bytes_total(st);

    bool need = false;
    MemoryCompactTrigger trig = MemoryCompactTrigger::auto_threshold;
    if (hard > 0 && bytes > hard) {
        need = true;
        trig = MemoryCompactTrigger::hard_cap;
    } else if (soft > 0 &&
               bytes >= static_cast<std::size_t>(static_cast<double>(soft) * ratio + 0.5)) {
        need = true;
        trig = MemoryCompactTrigger::auto_threshold;
    }
    if (!need) {
        return;
    }

    const int min_step = memory_auto_min_steps_from_env();
    if (st.iteration - st.last_memory_auto_compact_iteration < min_step) {
        return;
    }

    MemoryCompactResult r = run_memory_compaction(st, trig, opt);
    if (r.did_mutate) {
        st.last_memory_auto_compact_iteration = st.iteration;
    }
}

} // namespace agent_framework
