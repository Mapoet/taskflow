/**
 * @file openai_adapter.cpp
 * @brief OpenAI Chat Completions 适配器（流式 SSE / 非流式）
 */

#include <agent/agent_client/httplib_http_client.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/llm_client/llm_retry.hpp>

#include <algorithm>
#include <cstdint>
#include <future>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace agent_framework {

namespace {

std::string strip_trailing_slash(std::string s) {
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) {
        s.pop_back();
    }
    return s;
}

RenderedPrompt rendered_from_llm_input(const LLMInput& in) {
    RenderedPrompt rp;
    json msgs = json::array();
    if (!in.system_prompt.empty()) {
        msgs.push_back(json{{"role", "system"}, {"content", in.system_prompt}});
    }
    for (const auto& m : in.history) {
        json jm = json{{"role", m.role}, {"content", m.content}};
        if (m.role == "tool") {
            if (m.tool_name && !m.tool_name->empty()) {
                jm["name"] = *m.tool_name;
            }
        }
        msgs.push_back(std::move(jm));
    }
    std::string uc = in.user_prompt;
    if (!in.context.empty()) {
        uc = in.context + "\n\n" + uc;
    }
    msgs.push_back(json{{"role", "user"}, {"content", uc}});
    for (const auto& item : msgs) {
        rp.messages.push_back(item);
    }
    json tools = json::array();
    for (const auto& t : in.tools) {
        json fn = json{{"name", t.name}, {"description", t.description}};
        if (!t.schema.is_null()) {
            fn["parameters"] = t.schema;
        } else {
            fn["parameters"] = json::object();
        }
        tools.push_back(json{{"type", "function"}, {"function", std::move(fn)}});
    }
    rp.tools_json = std::move(tools);
    rp.cancellation_requested = in.cancellation_requested;
    rp.model_config = in.model_config;
    return rp;
}

std::optional<LLMUsage> parse_openai_usage(const json& body) {
    if(!body.is_object() || !body.contains("usage") || !body.at("usage").is_object())
        return std::nullopt;
    const auto& usage = body.at("usage");
    LLMUsage result;
    if(usage.contains("prompt_tokens") && usage.at("prompt_tokens").is_number_unsigned())
        result.input_tokens = usage.at("prompt_tokens").get<std::uint64_t>();
    else if(usage.contains("prompt_tokens") && usage.at("prompt_tokens").is_number_integer())
        result.input_tokens = static_cast<std::uint64_t>(std::max<std::int64_t>(0, usage.at("prompt_tokens").get<std::int64_t>()));
    if(usage.contains("completion_tokens") && usage.at("completion_tokens").is_number_unsigned())
        result.output_tokens = usage.at("completion_tokens").get<std::uint64_t>();
    else if(usage.contains("completion_tokens") && usage.at("completion_tokens").is_number_integer())
        result.output_tokens = static_cast<std::uint64_t>(std::max<std::int64_t>(0, usage.at("completion_tokens").get<std::int64_t>()));
    if(usage.contains("prompt_tokens_details") && usage.at("prompt_tokens_details").is_object()) {
        const auto& details = usage.at("prompt_tokens_details");
        if(details.contains("cached_tokens") && details.at("cached_tokens").is_number_integer())
            result.cached_input_tokens = static_cast<std::uint64_t>(std::max<std::int64_t>(0, details.at("cached_tokens").get<std::int64_t>()));
    }
    if(usage.contains("cost") && usage.at("cost").is_number()) result.cost_usd = usage.at("cost").get<double>();
    result.source = "provider:openai";
    if(!result.input_tokens && !result.output_tokens && !result.cost_usd)
        result.unknown_reason = "provider usage object contained no supported fields";
    return result;
}

LLMOutput parse_openai_non_stream(const json& body) {
    LLMOutput out;
    out.usage = parse_openai_usage(body);
    if (!body.contains("choices") || !body["choices"].is_array() || body["choices"].empty()) {
        out.is_final = true;
        return out;
    }
    const auto& choice = body["choices"][0];
    if (choice.contains("message")) {
        const auto& msg = choice["message"];
        if (msg.contains("reasoning") && msg["reasoning"].is_string()) {
            out.reasoning = msg["reasoning"].get<std::string>();
        }
        if (msg.contains("content")) {
            if (msg["content"].is_string()) {
                out.final_answer = msg["content"].get<std::string>();
            }
        }
        if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
            for (const auto& tc : msg["tool_calls"]) {
                CallSpec cs;
                if (tc.contains("id") && tc["id"].is_string()) {
                    cs.tool_call_id = tc["id"].get<std::string>();
                }
                if (tc.contains("function")) {
                    const auto& fn = tc["function"];
                    cs.name = fn.value("name", "");
                    std::string args = fn.value("arguments", "");
                    try {
                        cs.arguments = args.empty() ? json::object() : json::parse(args);
                    } catch (const json::exception&) {
                        cs.arguments = json::object();
                    }
                }
                out.tool_calls.push_back(std::move(cs));
            }
        }
    }
    out.is_final = out.tool_calls.empty();
    return out;
}

struct StreamToolSlot {
    std::string id;
    std::string name;
    std::string arguments;
};

std::vector<json> sanitize_openai_message_protocol(const std::vector<json>& messages) {
    std::vector<json> out;
    out.reserve(messages.size());
    for (std::size_t i = 0; i < messages.size();) {
        const json& message = messages[i];
        const std::string role = message.value("role", "");
        if (role == "tool") {
            ++i;
            continue;
        }
        if (role != "assistant" || !message.contains("tool_calls") ||
            !message.at("tool_calls").is_array() || message.at("tool_calls").empty()) {
            out.push_back(message);
            ++i;
            continue;
        }

        std::vector<std::string> expected;
        bool ids_valid = true;
        for (const auto& call : message.at("tool_calls")) {
            if (!call.is_object() || !call.contains("id") || !call.at("id").is_string() ||
                call.at("id").get_ref<const std::string&>().empty()) {
                ids_valid = false;
                break;
            }
            expected.push_back(call.at("id").get<std::string>());
        }
        std::unordered_set<std::string> remaining(expected.begin(), expected.end());
        std::vector<json> tools;
        std::size_t j = i + 1;
        while (j < messages.size() && messages[j].value("role", "") == "tool") {
            const json& tool = messages[j];
            if (tool.contains("tool_call_id") && tool.at("tool_call_id").is_string() &&
                remaining.erase(tool.at("tool_call_id").get<std::string>()) != 0U) {
                tools.push_back(tool);
            }
            ++j;
        }
        if (ids_valid && expected.size() == remaining.size() + tools.size() &&
            remaining.empty() && tools.size() == expected.size()) {
            out.push_back(message);
            out.insert(out.end(), tools.begin(), tools.end());
        }
        i = j;
    }
    return out;
}

} // namespace

OpenAIAdapter::OpenAIAdapter(const std::string& api_key, const std::string& base_url,
                             std::shared_ptr<LlmHttpTransport> transport)
    : api_key_(api_key),
      base_url_(strip_trailing_slash(base_url)),
      http_transport_(transport ? std::move(transport) : std::make_shared<HttplibLlmTransport>()) {}

void OpenAIAdapter::configure(const ModelConfig& config) {
    config_ = config;
    http_transport_->set_http_timeout_sec(config_.http_timeout_sec);
}

std::string OpenAIAdapter::get_model_name() const {
    return config_.model_name;
}

bool OpenAIAdapter::supports_multimodal() const {
    return true;
}

std::vector<ToolMeta> OpenAIAdapter::get_available_tools() const {
    return {};
}

json OpenAIAdapter::build_openai_request(const RenderedPrompt& rendered,
                                         const ModelConfig& config) {
    json req;
    req["model"] = config.model_name;
    json msgs = json::array();
    for (const auto& m : sanitize_openai_message_protocol(rendered.messages)) {
        msgs.push_back(m);
    }
    req["messages"] = std::move(msgs);
    if (!rendered.tools_json.is_null() && !rendered.tools_json.empty()) {
        if (rendered.tools_json.is_array()) {
            req["tools"] = rendered.tools_json;
        } else if (rendered.tools_json.is_object() && rendered.tools_json.contains("tools")) {
            req["tools"] = rendered.tools_json["tools"];
        } else {
            req["tools"] = rendered.tools_json;
        }
    }
    req["tool_choice"] = "auto";
    req["temperature"] = config.temperature;
    req["top_p"] = config.top_p;
    req["max_tokens"] = config.max_tokens;
    req["stream"] = config.stream;
    static const std::unordered_set<std::string> protected_fields = {
        "model", "messages", "tools", "tool_choice", "temperature", "top_p",
        "max_tokens", "stream"};
    for (const auto& kv : config.extra_params) {
        if(protected_fields.count(kv.first) == 0U) req[kv.first] = kv.second;
    }
    return req;
}

std::future<LLMOutput> OpenAIAdapter::invoke(
    const LLMInput& input,
    std::function<void(std::string_view)> stream_callback) {
    return invoke_with_rendered(rendered_from_llm_input(input), std::move(stream_callback));
}

std::future<LLMOutput> OpenAIAdapter::invoke_with_rendered(
    const RenderedPrompt& rendered,
    std::function<void(std::string_view)> stream_callback) {
    return invoke_with_rendered_channels(rendered, std::move(stream_callback), nullptr);
}

std::future<LLMOutput> OpenAIAdapter::invoke_with_rendered_channels(
    const RenderedPrompt& rendered,
    std::function<void(std::string_view)> answer_callback,
    std::function<void(std::string_view)> thinking_callback) {
    const char* dbg_env = std::getenv("AGENT_TEST_AGENT_LOOP_DEBUG");
    const bool dbg = dbg_env && std::string(dbg_env) != "0";
    const ModelConfig request_config = rendered.model_config.value_or(config_);
    return std::async(std::launch::async,
                      [this, rendered, request_config, cb = std::move(answer_callback),
                       thinking_cb = std::move(thinking_callback), dbg]() mutable {
        return invoke_with_retries(
            [this, &rendered, &request_config, &cb, &thinking_cb, dbg]() {
                http_transport_->set_http_timeout_sec(request_config.http_timeout_sec);
                const std::string url = base_url_ + "/chat/completions";
                json body = build_openai_request(rendered, request_config);
                std::map<std::string, std::string> hdrs = {{"Authorization", "Bearer " + api_key_},
                                                           {"Content-Type", "application/json"}};

                if (!request_config.stream) {
                    if (dbg) {
                        std::cout << "[OpenAIAdapter] POST " << url
                                  << " model=" << request_config.model_name
                                  << " stream=false timeout_sec=" << request_config.http_timeout_sec
                                  << " payload_chars=" << body.dump().size() << "\n";
                        std::cout.flush();
                    }
                    json resp = http_transport_->post_llm(url, body, hdrs, "openai");
                    return parse_openai_non_stream(resp);
                }

                body["stream"] = true;
                std::string full_text;
                std::map<int, StreamToolSlot> tool_acc;
                std::optional<LLMUsage> stream_usage;

                if (dbg) {
                    std::cout << "[OpenAIAdapter] SSE POST " << url
                              << " model=" << request_config.model_name
                              << " stream=true timeout_sec=" << request_config.http_timeout_sec
                              << " payload_chars=" << body.dump().size() << "\n";
                    std::cout.flush();
                }
                http_transport_->post_sse_cancellable(
                    url, body, hdrs,
                    [&](const std::string&, const json& ev) {
                        if(const auto usage = parse_openai_usage(ev)) stream_usage = usage;
                        if (!ev.contains("choices") || !ev["choices"].is_array()) {
                            return;
                        }
                        for (const auto& ch : ev["choices"]) {
                            if (!ch.contains("delta")) {
                                continue;
                            }
                            const auto& d = ch["delta"];
                            if (d.contains("content") && d["content"].is_string()) {
                                std::string piece = d["content"].get<std::string>();
                                full_text += piece;
                                if (cb) {
                                    cb(piece);
                                }
                            }
                            // Never expose provider raw reasoning_content/reasoning. Only fields
                            // explicitly labelled as a displayable summary enter the UI channel.
                            const char* summary_keys[] = {"reasoning_summary", "displayable_reasoning"};
                            for (const char* key : summary_keys) {
                                if (d.contains(key) && d[key].is_string() && thinking_cb) {
                                    thinking_cb(d[key].get_ref<const std::string&>());
                                    break;
                                }
                            }
                            if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
                                for (const auto& tc : d["tool_calls"]) {
                                    const int idx =
                                        tc.contains("index") && tc["index"].is_number_integer()
                                            ? static_cast<int>(tc["index"].get<std::int64_t>())
                                            : 0;
                                    if (tc.contains("id") && tc["id"].is_string()) {
                                        tool_acc[idx].id = tc["id"].get<std::string>();
                                    }
                                    if (tc.contains("function")) {
                                        const auto& fn = tc["function"];
                                        if (fn.contains("name") && fn["name"].is_string()) {
                                            tool_acc[idx].name = fn["name"].get<std::string>();
                                        }
                                        if (fn.contains("arguments") && fn["arguments"].is_string()) {
                                            tool_acc[idx].arguments += fn["arguments"].get<std::string>();
                                        }
                                    }
                                }
                            }
                        }
                    },
                    request_config.http_timeout_sec, "openai", rendered.cancellation_requested);

                LLMOutput out;
                out.final_answer = full_text;
                for (const auto& kv : tool_acc) {
                    const StreamToolSlot& sl = kv.second;
                    if (sl.name.empty() && sl.arguments.empty()) {
                        continue;
                    }
                    CallSpec cs;
                    cs.name = sl.name;
                    if (!sl.id.empty()) {
                        cs.tool_call_id = sl.id;
                    }
                    try {
                        cs.arguments =
                            sl.arguments.empty() ? json::object() : json::parse(sl.arguments);
                    } catch (const json::exception&) {
                        cs.arguments = json::object();
                    }
                    out.tool_calls.push_back(std::move(cs));
                }
                out.is_final = out.tool_calls.empty();
                out.usage = std::move(stream_usage);
                return out;
            },
            request_config);
    });
}

} // namespace agent_framework
