/**
 * @file openai_adapter.cpp
 * @brief OpenAI Chat Completions 适配器（流式 SSE / 非流式）
 */

#include "agent/httplib_http_client.hpp"
#include "agent/llm_client.hpp"
#include "agent/llm_retry.hpp"

#include <algorithm>
#include <cstdint>
#include <future>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
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
    return rp;
}

LLMOutput parse_openai_non_stream(const json& body) {
    LLMOutput out;
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

json OpenAIAdapter::build_openai_request(const RenderedPrompt& rendered) {
    json req;
    req["model"] = config_.model_name;
    json msgs = json::array();
    for (const auto& m : rendered.messages) {
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
    req["temperature"] = config_.temperature;
    req["max_tokens"] = config_.max_tokens;
    req["stream"] = config_.stream;
    for (const auto& kv : config_.extra_params) {
        req[kv.first] = kv.second;
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
    return std::async(std::launch::async, [this, rendered, cb = std::move(stream_callback)]() mutable {
        return invoke_with_retries(
            [this, &rendered, &cb]() {
                http_transport_->set_http_timeout_sec(config_.http_timeout_sec);
                const std::string url = base_url_ + "/chat/completions";
                json body = build_openai_request(rendered);
                std::map<std::string, std::string> hdrs = {{"Authorization", "Bearer " + api_key_},
                                                           {"Content-Type", "application/json"}};

                if (!config_.stream) {
                    json resp = http_transport_->post_llm(url, body, hdrs, "openai");
                    return parse_openai_non_stream(resp);
                }

                body["stream"] = true;
                std::string full_text;
                std::map<int, StreamToolSlot> tool_acc;

                http_transport_->post_sse(
                    url, body, hdrs,
                    [&](const std::string&, const json& ev) {
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
                    config_.http_timeout_sec, "openai");

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
                return out;
            },
            config_);
    });
}

} // namespace agent_framework
