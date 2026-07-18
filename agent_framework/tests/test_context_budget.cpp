/**
 * @file test_context_budget.cpp
 * @brief WP2.1c 上下文预算单元测试 B-1–B-5 与 PromptRenderer I-2
 */

#include <agent/context_budget/context_budget.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/core/types.hpp>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <stdlib.h>
#else
#include <unistd.h>
#endif

namespace {

using namespace agent_framework;

void set_env(const char* k, const char* v) {
#if defined(_WIN32)
    (void)_putenv_s(k, v);
#else
    (void)::setenv(k, v, 1);
#endif
}

void unset_env(const char* k) {
#if defined(_WIN32)
    (void)_putenv_s(k, "");
#else
    (void)::unsetenv(k);
#endif
}

void test_small_tool_result_no_truncation_meta() {
    ContextBudgetLimits L;
    L.max_tool_result_json_bytes = 4096;
    json j = json{{"ok", true}, {"n", 1}};
    apply_per_tool_result_budget(j, L, AfTruncationKind::tool_result);
    assert(!j.contains("_af_truncation"));
}

void test_b1_long_ascii_per_tool_cap() {
    ContextBudgetLimits L;
    L.max_tool_result_json_bytes = 512;
    L.spill_dir.clear();
    json j = json::object();
    j["payload"] = std::string(5000, 'a');
    apply_per_tool_result_budget(j, L, AfTruncationKind::tool_result);
    assert(j.contains("_af_truncation"));
    assert(j["_af_truncation"]["original_utf8_bytes"].get<std::size_t>() > 5000);
    assert(j["_af_truncation"]["kept_utf8_bytes"].get<std::size_t>() ==
           j["preview_text"].get<std::string>().size());
    assert(json_utf8_dump_bytes(j) <= L.max_tool_result_json_bytes);
}

void test_b2_utf8_boundary() {
    std::string u8 = "αβγδε";
    std::string s;
    for (int i = 0; i < 200; ++i) {
        s += u8;
    }
    const std::string t = utf8_safe_truncate(s, 13);
    assert(t.size() <= 13);
    assert(s.compare(0, t.size(), t) == 0);

    const std::string mixed = "A中🛰️B";
    assert(utf8_safe_truncate(mixed, 0).empty());
    assert(utf8_safe_truncate(mixed, 1) == "A");
    assert(utf8_safe_truncate(mixed, 2) == "A");
    assert(utf8_safe_truncate(mixed, 3) == "A");
    assert(utf8_safe_truncate(mixed, 4) == "A中");
    assert(utf8_safe_truncate(mixed, 5) == "A中");
    assert(utf8_safe_truncate(mixed, 6) == "A中");
    assert(utf8_safe_truncate(mixed, 7) == "A中");
    const json wire = {{"text", utf8_safe_truncate(mixed, 7)}};
    assert(!wire.dump().empty());
}

void test_b3_combined_earliest_tool_downgraded_first() {
    ContextBudgetLimits L;
    L.max_combined_prompt_attach_bytes = 1000;
    std::vector<Message> hist;
    Message a;
    a.role = "tool";
    a.tool_result = json{{"mark", 1}, {"x", std::string(520, 'a')}};
    hist.push_back(a);
    Message b;
    b.role = "tool";
    b.tool_result = json{{"mark", 2}, {"y", std::string(520, 'b')}};
    hist.push_back(b);
    ContextBudgetMeter::apply_combined_to_history_slice(hist, "", "", "", L);
    assert(hist[0].tool_result && is_combined_budget_stub(*hist[0].tool_result));
    assert(hist[1].tool_result && !is_combined_budget_stub(*hist[1].tool_result));
    assert((*hist[1].tool_result)["mark"].get<int>() == 2);
    const std::size_t sum = ContextBudgetMeter::combined_attach_bytes(hist, "", "", "");
    assert(sum <= L.max_combined_prompt_attach_bytes);
}

void test_b4_spill_file_when_dir_writable() {
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "af_ctx_budget_test_spill";
    std::filesystem::create_directories(tmp);
    ContextBudgetLimits L;
    L.max_tool_result_json_bytes = 400;
    L.spill_dir = tmp.string();
    json j = json{{"big", std::string(5000, 'z')}};
    apply_per_tool_result_budget(j, L, AfTruncationKind::tool_result);
    assert(j.contains("_af_truncation"));
    if (j["_af_truncation"]["spill"].get<bool>()) {
        const std::string ref = j["_af_truncation"]["ref"].get<std::string>();
        assert(!ref.empty());
        assert(std::filesystem::exists(tmp / ref));
    }
    std::filesystem::remove_all(tmp);
}

void test_b5_invalid_env_falls_back() {
#if !defined(_WIN32)
    set_env("AGENT_BUDGET_MAX_TOOL_RESULT_JSON_BYTES", "not-a-number");
    const ContextBudgetLimits L = ContextBudgetLimits::load(nullptr);
    assert(L.max_tool_result_json_bytes == 1048576);
    unset_env("AGENT_BUDGET_MAX_TOOL_RESULT_JSON_BYTES");
#endif
}

void test_i2_truncate_prompt_bytes_cap() {
    set_env("AGENT_BUDGET_MAX_RENDERED_MESSAGES_BYTES", "800");
    PromptRenderer pr;
    LLMInput in;
    in.system_prompt = "sys";
    in.user_prompt = "u";
    in.context = "";
    for (int i = 0; i < 15; ++i) {
        Message m;
        m.role = "user";
        m.content = std::string(200, static_cast<char>('0' + (i % 10)));
        in.history.push_back(m);
    }
    RenderedPrompt r = pr.render(in, "gpt-test");
    unset_env("AGENT_BUDGET_MAX_RENDERED_MESSAGES_BYTES");
    std::size_t total = 0;
    for (const auto& m : r.messages) {
        total += json_utf8_dump_bytes(m);
    }
    assert(total <= 900);
}

} // namespace

int main() {
    test_small_tool_result_no_truncation_meta();
    test_b1_long_ascii_per_tool_cap();
    test_b2_utf8_boundary();
    test_b3_combined_earliest_tool_downgraded_first();
    test_b4_spill_file_when_dir_writable();
    test_b5_invalid_env_falls_back();
    test_i2_truncate_prompt_bytes_cap();
    return 0;
}
