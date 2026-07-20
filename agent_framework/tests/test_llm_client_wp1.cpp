/**
 * @file test_llm_client_wp1.cpp
 * @brief WP1.1 LLMClient 测试
 *
 * **Live（DeepSeek/OpenAI 兼容 + Anthropic 兼容）**：设置 `DEEPSEEK_API_KEY`（见 `~/.bashrc`）。
 * - OpenAI 风格：`AGENT_OPENAI_BASE_URL` 默认 `https://api.deepseek.com/v1`
 * - Anthropic 风格：`AGENT_ANTHROPIC_BASE_URL` 默认 `https://api.deepseek.com/anthropic`
 * - 模型：`DEEPSEEK_MODEL` 默认 `deepseek-chat`
 *
 * **Offline**：`AGENT_TEST_OFFLINE=1` 且 ctest 已设置 `AGENT_TEST_FIXTURES`，走假 transport + 夹具。
 *
 * **Live 输出**：默认把每次 API 的 `final_answer` / `tool_calls` / 流式累积打印到 **stdout**。
 * 若需静默（如 CI），设 `AGENT_TEST_QUIET=1`。
 */

#include <agent/agent_client/httplib_http_client.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/llm_client/llm_retry.hpp>

#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;
using namespace agent_framework;

std::string env_or(const char* key, const char* default_val) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : std::string(default_val);
}

bool env_truthy(const char* key) {
    const char* v = std::getenv(key);
    if (!v || !*v) {
        return false;
    }
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T';
}

/** Live 下是否跳过打印模型输出（ctest 可设 AGENT_TEST_QUIET=1） */
bool live_output_quiet() {
    return env_truthy("AGENT_TEST_QUIET");
}

void live_print_case(const char* title) {
    if (live_output_quiet()) {
        return;
    }
    std::cout << "\n========== " << title << " ==========\n";
}

void live_print_output(const LLMOutput& o, const std::string* streamed = nullptr) {
    if (live_output_quiet()) {
        return;
    }
    std::cout << "is_final: " << (o.is_final ? "true" : "false") << '\n';
    if (!o.reasoning.empty()) {
        std::cout << "reasoning: " << o.reasoning << '\n';
    }
    if (streamed && !streamed->empty()) {
        std::cout << "stream accumulated (" << streamed->size() << " chars): " << *streamed << '\n';
    }
    if (!o.final_answer.empty()) {
        std::cout << "final_answer:\n" << o.final_answer << '\n';
    }
    if (!o.tool_calls.empty()) {
        std::cout << "tool_calls (" << o.tool_calls.size() << "):\n";
        for (std::size_t i = 0; i < o.tool_calls.size(); ++i) {
            const CallSpec& tc = o.tool_calls[i];
            std::cout << "  [" << i << "] name=\"" << tc.name << '"';
            if (tc.tool_call_id.has_value() && !tc.tool_call_id->empty()) {
                std::cout << " tool_call_id=\"" << *tc.tool_call_id << '"';
            }
            std::cout << "\n      arguments: " << tc.arguments.dump() << '\n';
        }
    }
    std::cout.flush();
}

std::string fixtures_base_dir() {
    const char* e = std::getenv("AGENT_TEST_FIXTURES");
    return e ? std::string(e) : std::string();
}

std::string read_all(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("read_all: cannot open " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

json tool_add_openai_array() {
    json params = json::parse(
        R"({"type":"object","properties":{"a":{"type":"integer","description":"first"},"b":{"type":"integer","description":"second"}},"required":["a","b"]})");
    return json::array(
        {{{"type", "function"},
          {"function",
           {{"name", "add"},
            {"description", "Return the sum of integers a and b."},
            {"parameters", std::move(params)}}}}});
}

json assistant_message_from_openai_tool_calls(const LLMOutput& o) {
    json msg;
    msg["role"] = "assistant";
    msg["content"] = nullptr;
    json arr = json::array();
    for (const auto& tc : o.tool_calls) {
        json item;
        item["id"] = tc.tool_call_id.value_or("");
        item["type"] = "function";
        item["function"] =
            json{{"name", tc.name}, {"arguments", tc.arguments.empty() ? "{}" : tc.arguments.dump()}};
        arr.push_back(std::move(item));
    }
    msg["tool_calls"] = std::move(arr);
    return msg;
}

json tool_message_openai(const std::string& tool_call_id, std::string content) {
    return json{{"role", "tool"}, {"tool_call_id", tool_call_id}, {"content", std::move(content)}};
}

int add_args_sum(const CallSpec& tc) {
    const auto& a = tc.arguments.at("a");
    const auto& b = tc.arguments.at("b");
    int ai = a.is_number_integer() ? static_cast<int>(a.get<std::int64_t>()) : static_cast<int>(a.get<double>());
    int bi = b.is_number_integer() ? static_cast<int>(b.get<std::int64_t>()) : static_cast<int>(b.get<double>());
    return ai + bi;
}

void assert_contains_digit_sum(const std::string& text, int expected) {
    if (text.find(std::to_string(expected)) != std::string::npos) {
        return;
    }
    throw std::runtime_error("expected answer to contain " + std::to_string(expected) + ", got: " +
                             text.substr(0, 200));
}

// --- Offline fake transport (fixtures) ---

class FakeLlmTransport : public LlmHttpTransport {
public:
    json post_response;
    std::vector<json> sse_chunks;
    int post_llm_calls = 0;
    int fail_post_with_503 = 0;
    int cancellable_sse_calls = 0;
    json last_request;

    json post_llm(const std::string&, const json& request, const std::map<std::string, std::string>&,
                  const std::string&) override {
        ++post_llm_calls;
        last_request = request;
        if (fail_post_with_503 > 0) {
            --fail_post_with_503;
            throw llm_http_error(503, "test", "service unavailable", std::nullopt);
        }
        return post_response;
    }

    void post_sse(const std::string&, const json&, const std::map<std::string, std::string>& headers,
                  const std::function<void(const std::string&, const json&)>& on_event, int,
                  const std::string&) override {
        (void)headers;
        for (const auto& ch : sse_chunks) {
            on_event("", ch);
        }
    }

    void post_sse_cancellable(
        const std::string&, const json&, const std::map<std::string, std::string>&,
        const std::function<void(const std::string&, const json&)>& on_event, int,
        const std::string&, const std::function<bool()>& cancellation_requested) override {
        ++cancellable_sse_calls;
        for (const auto& chunk : sse_chunks) {
            if (cancellation_requested && cancellation_requested()) break;
            on_event("", chunk);
        }
    }
};

json load_fixture(const char* name) {
    const std::string base = fixtures_base_dir();
    if (base.empty()) {
        throw std::runtime_error("AGENT_TEST_FIXTURES env not set");
    }
    return json::parse(read_all(base + "/" + name));
}

void test_openai_nonstream_text() {
    auto fake = std::make_shared<FakeLlmTransport>();
    fake->post_response = load_fixture("openai_nonstream_text.json");
    OpenAIAdapter ad("sk-test", "https://api.example.com/v1", fake);
    ModelConfig cfg;
    cfg.model_name = "gpt-test";
    cfg.stream = false;
    ad.configure(cfg);
    RenderedPrompt rp;
    rp.messages = {{{"role", "user"}, {"content", "hi"}}};
    LLMOutput out = ad.invoke_with_rendered(rp, nullptr).get();
    assert(out.final_answer == "Hello from fixture.");
    assert(out.tool_calls.empty());
    assert(out.is_final);
}

void test_openai_nonstream_tools() {
    auto fake = std::make_shared<FakeLlmTransport>();
    fake->post_response = load_fixture("openai_nonstream_tools.json");
    OpenAIAdapter ad("sk-test", "https://api.example.com/v1", fake);
    ModelConfig cfg;
    cfg.model_name = "gpt-test";
    cfg.stream = false;
    ad.configure(cfg);
    RenderedPrompt rp;
    rp.messages = {{{"role", "user"}, {"content", "weather?"}}};
    LLMOutput out = ad.invoke_with_rendered(rp, nullptr).get();
    assert(out.tool_calls.size() == 1u);
    assert(out.tool_calls[0].name == "get_weather");
    assert(out.tool_calls[0].arguments["city"] == "Paris");
    assert(!out.is_final);
}

void test_openai_stream_text() {
    auto fake = std::make_shared<FakeLlmTransport>();
    fake->sse_chunks = {
        json::parse(R"({"choices":[{"delta":{"content":"Hel"}}]})"),
        json::parse(R"({"choices":[{"delta":{"content":"lo"}}]})"),
    };
    OpenAIAdapter ad("sk-test", "https://api.example.com/v1", fake);
    ModelConfig cfg;
    cfg.model_name = "gpt-test";
    cfg.stream = true;
    ad.configure(cfg);
    RenderedPrompt rp;
    rp.messages = {{{"role", "user"}, {"content", "hi"}}};
    std::string acc;
    LLMOutput out = ad.invoke_with_rendered(rp, [&](std::string_view s) { acc += s; }).get();
    assert(acc == "Hello");
    assert(out.final_answer == "Hello");
    assert(out.is_final);
}

void test_openai_typed_stream_separates_displayable_summary() {
    auto fake = std::make_shared<FakeLlmTransport>();
    fake->sse_chunks = {
        json::parse(R"({"choices":[{"delta":{"reasoning_content":"private","reasoning_summary":"checked"}}]})"),
        json::parse(R"({"choices":[{"delta":{"content":"answer"}}]})"),
    };
    OpenAIAdapter adapter("sk-test", "https://api.example.com/v1", fake);
    ModelConfig config; config.model_name = "gpt-test"; config.stream = true; adapter.configure(config);
    RenderedPrompt prompt; prompt.messages = {{{"role", "user"}, {"content", "hi"}}};
    std::string answer;
    std::string thinking;
    const auto output = adapter.invoke_with_rendered_channels(
        prompt, [&](std::string_view value) { answer += value; },
        [&](std::string_view value) { thinking += value; }).get();
    assert(answer == "answer");
    assert(thinking == "checked");
    assert(thinking.find("private") == std::string::npos);
    assert(output.final_answer == "answer");
}

void test_openai_stream_transport_cancellation() {
    auto fake = std::make_shared<FakeLlmTransport>();
    fake->sse_chunks = {
        json::parse(R"({"choices":[{"delta":{"content":"first"}}]})"),
        json::parse(R"({"choices":[{"delta":{"content":"second"}}]})"),
    };
    OpenAIAdapter adapter("sk-test", "https://api.example.com/v1", fake);
    ModelConfig config;
    config.model_name = "gpt-test";
    config.stream = true;
    adapter.configure(config);
    bool cancelled = false;
    RenderedPrompt prompt;
    prompt.messages = {{{"role", "user"}, {"content", "hi"}}};
    prompt.cancellation_requested = [&] { return cancelled; };
    std::string received;
    LLMOutput output = adapter.invoke_with_rendered(
        prompt, [&](std::string_view chunk) {
            received += chunk;
            cancelled = true;
        }).get();
    assert(fake->cancellable_sse_calls == 1);
    assert(received == "first");
    assert(output.final_answer == "first");
}

void test_openai_stream_tools() {
    auto fake = std::make_shared<FakeLlmTransport>();
    fake->sse_chunks = {
        json::parse(
            R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"c1","function":{"name":"fn1","arguments":""}}]}}]})"),
        json::parse(
            R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"x\":"}}]}}]})"),
        json::parse(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"1}"}}]}}]})"),
    };
    OpenAIAdapter ad("sk-test", "https://api.example.com/v1", fake);
    ModelConfig cfg;
    cfg.model_name = "gpt-test";
    cfg.stream = true;
    ad.configure(cfg);
    RenderedPrompt rp;
    rp.messages = {{{"role", "user"}, {"content", "call tool"}}};
    LLMOutput out = ad.invoke_with_rendered(rp, nullptr).get();
    assert(out.tool_calls.size() == 1u);
    assert(out.tool_calls[0].tool_call_id.has_value() && *out.tool_calls[0].tool_call_id == "c1");
    assert(out.tool_calls[0].name == "fn1");
    assert(out.tool_calls[0].arguments["x"] == 1);
    assert(!out.is_final);
}

void test_anthropic_nonstream() {
    auto fake = std::make_shared<FakeLlmTransport>();
    fake->post_response = load_fixture("anthropic_nonstream.json");
    AnthropicAdapter ad("sk-ant", "https://api.anth.example", fake);
    ModelConfig cfg;
    cfg.model_name = "claude-test";
    cfg.stream = false;
    ad.configure(cfg);
    RenderedPrompt rp;
    rp.messages = {{{"role", "user"}, {"content", "go"}}};
    LLMOutput out = ad.invoke_with_rendered(rp, nullptr).get();
    assert(out.final_answer == "Anthropic says hi.");
    assert(out.tool_calls.size() == 1u);
    assert(out.tool_calls[0].tool_call_id.has_value() && *out.tool_calls[0].tool_call_id == "toolu_01");
    assert(out.tool_calls[0].name == "calc");
    assert(out.tool_calls[0].arguments["expr"] == "1+1");
    assert(!out.is_final);
}

void test_anthropic_stream() {
    auto fake = std::make_shared<FakeLlmTransport>();
    fake->sse_chunks = {
        {{"type", "content_block_start"}, {"index", 0}, {"content_block", {{"type", "text"}}}},
        {{"type", "content_block_delta"},
         {"index", 0},
         {"delta", {{"type", "text_delta"}, {"text", "Hi "}}}},
        {{"type", "content_block_delta"},
         {"index", 0},
         {"delta", {{"type", "text_delta"}, {"text", "there"}}}},
        {{"type", "content_block_start"},
         {"index", 1},
         {"content_block", {{"type", "tool_use"}, {"id", "tu1"}, {"name", "add"}}}},
        {{"type", "content_block_delta"},
         {"index", 1},
         {"delta", {{"type", "input_json_delta"}, {"partial_json", "{\"a\":"}}}},
        {{"type", "content_block_delta"},
         {"index", 1},
         {"delta", {{"type", "input_json_delta"}, {"partial_json", "2}"}}}},
    };
    AnthropicAdapter ad("k", "https://api.anthropic.example", fake);
    ModelConfig cfg;
    cfg.model_name = "claude-test";
    cfg.stream = true;
    ad.configure(cfg);
    RenderedPrompt rp;
    rp.messages = {{{"role", "user"}, {"content", "x"}}};
    std::string stream_acc;
    LLMOutput out = ad.invoke_with_rendered(rp, [&](std::string_view s) { stream_acc += s; }).get();
    assert(stream_acc == "Hi there");
    assert(out.final_answer == "Hi there");
    assert(out.tool_calls.size() == 1u);
    assert(out.tool_calls[0].tool_call_id.has_value() && *out.tool_calls[0].tool_call_id == "tu1");
    assert(out.tool_calls[0].name == "add");
    assert(out.tool_calls[0].arguments["a"] == 2);
    assert(!out.is_final);
}

void test_invoke_with_retries_eventually_ok() {
    auto fake = std::make_shared<FakeLlmTransport>();
    fake->fail_post_with_503 = 1;
    fake->post_response = load_fixture("openai_nonstream_text.json");
    OpenAIAdapter ad("k", "https://api.example.com/v1", fake);
    ModelConfig cfg;
    cfg.model_name = "m";
    cfg.stream = false;
    cfg.max_retries = 3;
    ad.configure(cfg);
    RenderedPrompt rp;
    rp.messages = {{{"role", "user"}, {"content", "."}}};
    LLMOutput out = ad.invoke_with_rendered(rp, nullptr).get();
    assert(out.is_final);
    assert(fake->post_llm_calls == 2);
}

void test_llm_client_invoke_with_rendered() {
    auto fake = std::make_shared<FakeLlmTransport>();
    fake->post_response = load_fixture("openai_nonstream_text.json");
    auto adapter = std::make_shared<OpenAIAdapter>("k", "https://api.example.com/v1", fake);
    ModelConfig cfg;
    cfg.model_name = "m";
    cfg.stream = false;
    adapter->configure(cfg);
    LLMClient client;
    client.register_adapter("openai", adapter);
    client.set_default_adapter("openai");
    RenderedPrompt rp;
    rp.messages = {{{"role", "user"}, {"content", "?"}}};
    LLMOutput out = client.invoke_with_rendered_prompt(rp, "openai", nullptr).get();
    assert(out.final_answer.find("Hello") != std::string::npos);
}

void test_openai_request_repairs_incomplete_tool_group() {
    auto fake = std::make_shared<FakeLlmTransport>();
    fake->post_response = load_fixture("openai_nonstream_text.json");
    OpenAIAdapter adapter("k", "https://api.example.com/v1", fake);
    ModelConfig config;
    config.model_name = "m";
    config.stream = false;
    adapter.configure(config);

    RenderedPrompt prompt;
    prompt.messages = {
        json{{"role", "user"}, {"content", "old"}},
        json{{"role", "assistant"},
             {"content", nullptr},
             {"tool_calls",
              json::array({json{{"id", "c1"},
                                {"type", "function"},
                                {"function", {{"name", "x"}, {"arguments", "{}"}}}},
                           json{{"id", "c2"},
                                {"type", "function"},
                                {"function", {{"name", "x"}, {"arguments", "{}"}}}}})}},
        json{{"role", "tool"}, {"tool_call_id", "c1"}, {"content", "{}"}},
        json{{"role", "user"}, {"content", "new"}}};
    (void)adapter.invoke_with_rendered(prompt, nullptr).get();
    assert(fake->last_request.contains("messages"));
    const json& sent = fake->last_request.at("messages");
    assert(sent.size() == 2U);
    assert(sent[0].at("content") == "old");
    assert(sent[1].at("content") == "new");
}

void run_offline_tests() {
    test_openai_nonstream_text();
    test_openai_nonstream_tools();
    test_openai_stream_text();
    test_openai_typed_stream_separates_displayable_summary();
    test_openai_stream_transport_cancellation();
    test_openai_stream_tools();
    test_anthropic_nonstream();
    test_anthropic_stream();
    test_invoke_with_retries_eventually_ok();
    test_llm_client_invoke_with_rendered();
    test_openai_request_repairs_incomplete_tool_group();
}

ModelConfig live_model_config(const std::string& model) {
    ModelConfig cfg;
    cfg.model_name = model;
    cfg.stream = false;
    cfg.temperature = 0.0;
    cfg.max_tokens = 512;
    cfg.max_retries = 2;
    cfg.http_timeout_sec = 180;
    return cfg;
}

const char* live_api_key() {
    const char* k = std::getenv("DEEPSEEK_API_KEY");
    if (k && *k) {
        return k;
    }
    return std::getenv("OPENAI_API_KEY");
}

void run_live_openai_nonstream_text() {
    live_print_case("OpenAI non-stream (chat)");
    const char* key = live_api_key();
    OpenAIAdapter ad(key, env_or("AGENT_OPENAI_BASE_URL", "https://api.deepseek.com/v1"));
    ModelConfig cfg = live_model_config(env_or("DEEPSEEK_MODEL", "deepseek-chat"));
    ad.configure(cfg);
    RenderedPrompt rp;
    rp.messages = {json{{"role", "user"},
                          {"content", "Reply with exactly one English word: the color of the sky on a clear day."}}};
    LLMOutput out = ad.invoke_with_rendered(rp, nullptr).get();
    live_print_output(out, nullptr);
    if (out.final_answer.empty()) {
        throw std::runtime_error("live openai: empty final_answer");
    }
    assert(out.tool_calls.empty());
    assert(out.is_final);
}

void run_live_openai_tool_roundtrip() {
    live_print_case("OpenAI tool round-trip (turn 1: model calls add)");
    const char* key = live_api_key();
    OpenAIAdapter ad(key, env_or("AGENT_OPENAI_BASE_URL", "https://api.deepseek.com/v1"));
    ModelConfig cfg = live_model_config(env_or("DEEPSEEK_MODEL", "deepseek-chat"));
    ad.configure(cfg);
    RenderedPrompt rp1;
    rp1.tools_json = tool_add_openai_array();
    rp1.messages = {json{{"role", "user"},
                          {"content",
                           "You must use the add tool to compute 11 + 31. "
                           "Call add with a=11 and b=31 only. Do not state the sum before the tool runs."}}};
    LLMOutput first = ad.invoke_with_rendered(rp1, nullptr).get();
    live_print_output(first, nullptr);
    if (first.tool_calls.empty()) {
        throw std::runtime_error("live openai tool: model did not return tool_calls (enable function calling?)");
    }
    const CallSpec& tc = first.tool_calls[0];
    if (!tc.tool_call_id.has_value() || tc.tool_call_id->empty()) {
        throw std::runtime_error("live openai tool: missing tool_call id from API");
    }
    if (tc.name != "add") {
        throw std::runtime_error("live openai tool: expected tool add, got " + tc.name);
    }
    const int sum = add_args_sum(tc);
    if (!live_output_quiet()) {
        std::cout << "injected tool_result content (string): " << sum << '\n';
    }
    json assistant_msg = assistant_message_from_openai_tool_calls(first);
    json tool_msg = tool_message_openai(*tc.tool_call_id, std::to_string(sum));

    RenderedPrompt rp2;
    rp2.tools_json = tool_add_openai_array();
    rp2.messages = {rp1.messages[0], assistant_msg, tool_msg};

    live_print_case("OpenAI tool round-trip (turn 2: after tool_result)");
    LLMOutput second = ad.invoke_with_rendered(rp2, nullptr).get();
    live_print_output(second, nullptr);
    if (!second.is_final || second.final_answer.empty()) {
        throw std::runtime_error("live openai tool round 2: expected final text answer");
    }
    assert_contains_digit_sum(second.final_answer, sum);
}

void run_live_openai_stream() {
    live_print_case("OpenAI stream (chat)");
    const char* key = live_api_key();
    OpenAIAdapter ad(key, env_or("AGENT_OPENAI_BASE_URL", "https://api.deepseek.com/v1"));
    ModelConfig cfg = live_model_config(env_or("DEEPSEEK_MODEL", "deepseek-chat"));
    cfg.stream = true;
    ad.configure(cfg);
    RenderedPrompt rp;
    rp.messages = {json{{"role", "user"}, {"content", "Count from 1 to 3 separated by commas, no extra words."}}};
    std::string streamed;
    LLMOutput out = ad.invoke_with_rendered(rp, [&](std::string_view s) { streamed += s; }).get();
    live_print_output(out, &streamed);
    if (streamed.empty() && out.final_answer.empty()) {
        throw std::runtime_error("live openai stream: no tokens");
    }
}

void run_live_openai_stream_tools() {
    live_print_case("OpenAI stream + tools");
    const char* key = live_api_key();
    OpenAIAdapter ad(key, env_or("AGENT_OPENAI_BASE_URL", "https://api.deepseek.com/v1"));
    ModelConfig cfg = live_model_config(env_or("DEEPSEEK_MODEL", "deepseek-chat"));
    cfg.stream = true;
    ad.configure(cfg);
    RenderedPrompt rp;
    rp.tools_json = tool_add_openai_array();
    rp.messages = {json{{"role", "user"},
                          {"content", "Use add with a=2 and b=3 only via the tool."}}};
    std::string streamed;
    LLMOutput out = ad.invoke_with_rendered(rp, [&](std::string_view s) { streamed += s; }).get();
    live_print_output(out, &streamed);
    if (out.tool_calls.empty() && out.final_answer.empty()) {
        throw std::runtime_error("live openai stream+tools: no output");
    }
    if (!out.tool_calls.empty() && out.tool_calls[0].tool_call_id.has_value()) {
        assert(!out.tool_calls[0].tool_call_id->empty());
    }
}

void run_live_anthropic_nonstream_text() {
    live_print_case("Anthropic non-stream (Messages API)");
    const char* key = live_api_key();
    if (!key || !*key) {
        throw std::runtime_error("live anthropic needs DEEPSEEK_API_KEY");
    }
    AnthropicAdapter ad(key, env_or("AGENT_ANTHROPIC_BASE_URL", "https://api.deepseek.com/anthropic"));
    ModelConfig cfg = live_model_config(env_or("DEEPSEEK_MODEL", "deepseek-chat"));
    ad.configure(cfg);
    RenderedPrompt rp;
    rp.messages = {json{{"role", "user"},
                          {"content", "Reply with one word: capital of France."}}};
    LLMOutput out = ad.invoke_with_rendered(rp, nullptr).get();
    live_print_output(out, nullptr);
    if (out.final_answer.empty() && out.tool_calls.empty()) {
        throw std::runtime_error("live anthropic: empty response");
    }
}

void run_live_anthropic_tool_roundtrip() {
    live_print_case("Anthropic tool round-trip (turn 1)");
    const char* key = live_api_key();
    AnthropicAdapter ad(key, env_or("AGENT_ANTHROPIC_BASE_URL", "https://api.deepseek.com/anthropic"));
    ModelConfig cfg = live_model_config(env_or("DEEPSEEK_MODEL", "deepseek-chat"));
    ad.configure(cfg);
    RenderedPrompt rp1;
    rp1.tools_json = tool_add_openai_array();
    rp1.messages = {json{{"role", "user"},
                          {"content",
                           "Use the add tool with a=7 and b=35 only. "
                           "Do not give the numeric answer before calling add."}}};
    LLMOutput first = ad.invoke_with_rendered(rp1, nullptr).get();
    live_print_output(first, nullptr);
    if (first.tool_calls.empty()) {
        throw std::runtime_error("live anthropic tool: no tool_calls from model");
    }
    const CallSpec& tc = first.tool_calls[0];
    if (!tc.tool_call_id.has_value() || tc.tool_call_id->empty()) {
        throw std::runtime_error("live anthropic tool: missing tool_use id");
    }
    if (tc.name != "add") {
        throw std::runtime_error("live anthropic tool: expected add, got " + tc.name);
    }
    const int sum = add_args_sum(tc);
    if (!live_output_quiet()) {
        std::cout << "injected tool_result content (string): " << sum << '\n';
    }
    json assistant_msg = assistant_message_from_openai_tool_calls(first);
    json tool_msg = tool_message_openai(*tc.tool_call_id, std::to_string(sum));

    RenderedPrompt rp2;
    rp2.tools_json = tool_add_openai_array();
    rp2.messages = {rp1.messages[0], assistant_msg, tool_msg};

    live_print_case("Anthropic tool round-trip (turn 2)");
    LLMOutput second = ad.invoke_with_rendered(rp2, nullptr).get();
    live_print_output(second, nullptr);
    if (!second.is_final || second.final_answer.empty()) {
        throw std::runtime_error("live anthropic round 2: expected final answer");
    }
    assert_contains_digit_sum(second.final_answer, sum);
}

void run_live_anthropic_stream() {
    live_print_case("Anthropic stream (chat)");
    const char* key = live_api_key();
    AnthropicAdapter ad(key, env_or("AGENT_ANTHROPIC_BASE_URL", "https://api.deepseek.com/anthropic"));
    ModelConfig cfg = live_model_config(env_or("DEEPSEEK_MODEL", "deepseek-chat"));
    cfg.stream = true;
    ad.configure(cfg);
    RenderedPrompt rp;
    rp.messages = {json{{"role", "user"}, {"content", "Say hi in 2 words."}}};
    std::string acc;
    LLMOutput out = ad.invoke_with_rendered(rp, [&](std::string_view s) { acc += s; }).get();
    live_print_output(out, &acc);
    if (acc.empty() && out.final_answer.empty()) {
        throw std::runtime_error("live anthropic stream: empty");
    }
}

void run_live_anthropic_stream_tools() {
    live_print_case("Anthropic stream + tools");
    const char* key = live_api_key();
    AnthropicAdapter ad(key, env_or("AGENT_ANTHROPIC_BASE_URL", "https://api.deepseek.com/anthropic"));
    ModelConfig cfg = live_model_config(env_or("DEEPSEEK_MODEL", "deepseek-chat"));
    cfg.stream = true;
    ad.configure(cfg);
    RenderedPrompt rp;
    rp.tools_json = tool_add_openai_array();
    rp.messages = {json{
        {"role", "user"}, {"content", "Use add for a=1 b=1 via tool only."}}};
    std::string acc;
    LLMOutput out = ad.invoke_with_rendered(rp, [&](std::string_view s) { acc += s; }).get();
    live_print_output(out, &acc);
    if (out.tool_calls.empty() && out.final_answer.empty() && acc.empty()) {
        throw std::runtime_error("live anthropic stream+tools: empty");
    }
}

void run_live_llm_client_invoke() {
    live_print_case("LLMClient::invoke_with_rendered_prompt (OpenAI adapter)");
    const char* key = live_api_key();
    auto adapter = std::make_shared<OpenAIAdapter>(
        key, env_or("AGENT_OPENAI_BASE_URL", "https://api.deepseek.com/v1"));
    adapter->configure(live_model_config(env_or("DEEPSEEK_MODEL", "deepseek-chat")));
    LLMClient client;
    client.register_adapter("openai", adapter);
    client.set_default_adapter("openai");
    RenderedPrompt rp;
    rp.messages = {json{{"role", "user"}, {"content", "Reply OK if you read this."}}};
    LLMOutput out = client.invoke_with_rendered_prompt(rp, "openai", nullptr).get();
    live_print_output(out, nullptr);
    if (out.final_answer.empty()) {
        throw std::runtime_error("live LLMClient: empty reply");
    }
}

void run_live_tests() {
    run_live_openai_nonstream_text();
    run_live_openai_tool_roundtrip();
    run_live_openai_stream();
    run_live_openai_stream_tools();
    run_live_anthropic_nonstream_text();
    run_live_anthropic_tool_roundtrip();
    run_live_anthropic_stream();
    run_live_anthropic_stream_tools();
    run_live_llm_client_invoke();
}

} // namespace

int main() {
    try {
        if (live_api_key()) {
            std::cerr << "test_llm_client_wp1: live (DeepSeek/OpenAI + Anthropic endpoints)\n";
            std::cerr << "(model output on stdout; set AGENT_TEST_QUIET=1 to suppress)\n";
            run_live_tests();
            std::cerr << "\ntest_llm_client_wp1: ok (live)\n";
            return 0;
        }
        if (env_truthy("AGENT_TEST_OFFLINE") && !fixtures_base_dir().empty()) {
            run_offline_tests();
            std::cerr << "test_llm_client_wp1: ok (offline)\n";
            return 0;
        }
        std::cerr
            << "SKIP: set DEEPSEEK_API_KEY for live tests (see ~/.bashrc), or AGENT_TEST_OFFLINE=1 with "
               "AGENT_TEST_FIXTURES for fixtures.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
