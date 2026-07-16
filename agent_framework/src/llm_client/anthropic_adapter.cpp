/**
 * @file anthropic_adapter.cpp
 * @brief Anthropic Messages API 适配器
 */

#include <agent/agent_client/httplib_http_client.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/llm_client/llm_retry.hpp>

#include <algorithm>
#include <future>
#include <map>
#include <stdexcept>
#include <string>

namespace agent_framework {

namespace {

/** 与官方 Messages API 对齐；合并前请以当前文档复核 */
constexpr const char* k_anthropic_api_version = "2023-06-01";
constexpr int k_anthropic_min_max_tokens = 1024;

std::string strip_trailing_slash(std::string s) {
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) {
        s.pop_back();
    }
    return s;
}

json openai_tools_to_anthropic(const json& tools_json) {
    json out = json::array();
    if (tools_json.is_null() || tools_json.empty()) {
        return out;
    }
    json src = tools_json;
    if (tools_json.is_object() && tools_json.contains("tools")) {
        src = tools_json["tools"];
    }
    if (!src.is_array()) {
        return out;
    }
    for (const auto& t : src) {
        if (t.contains("name") && t.contains("input_schema") && !t.contains("function")) {
            out.push_back(t);
            continue;
        }
        if (!t.contains("function")) {
            continue;
        }
        const auto& f = t["function"];
        json tool = {{"name", f.value("name", "")},
                     {"description", f.value("description", "")},
                     {"input_schema",
                      f.contains("parameters") && !f["parameters"].is_null() ? f["parameters"]
                                                                           : json::object()}};
        out.push_back(std::move(tool));
    }
    return out;
}

json user_content_from_openai_message(const json& m) {
    if (!m.contains("content")) {
        return json::array({{{"type", "text"}, {"text", ""}}});
    }
    const auto& c = m["content"];
    if (c.is_string()) {
        return json::array({{{"type", "text"}, {"text", c.get<std::string>()}}});
    }
    if (c.is_array()) {
        json blocks = json::array();
        for (const auto& part : c) {
            blocks.push_back(part);
        }
        if (blocks.empty()) {
            blocks.push_back({{"type", "text"}, {"text", ""}});
        }
        return blocks;
    }
    return json::array({{{"type", "text"}, {"text", c.dump()}}});
}

RenderedPrompt rendered_from_llm_input(const LLMInput& in) {
    RenderedPrompt rp;
    if (!in.system_prompt.empty()) {
        rp.messages.push_back(json{{"role", "system"}, {"content", in.system_prompt}});
    }
    for (const auto& m : in.history) {
        json jm = json{{"role", m.role}, {"content", m.content}};
        rp.messages.push_back(std::move(jm));
    }
    std::string uc = in.user_prompt;
    if (!in.context.empty()) {
        uc = in.context + "\n\n" + uc;
    }
    rp.messages.push_back(json{{"role", "user"}, {"content", uc}});
    json tools = json::array();
    for (const auto& t : in.tools) {
        json fn = json{{"name", t.name}, {"description", t.description}};
        fn["parameters"] = t.schema.is_null() ? json::object() : t.schema;
        tools.push_back(json{{"type", "function"}, {"function", std::move(fn)}});
    }
    rp.tools_json = std::move(tools);
    return rp;
}

LLMOutput parse_anthropic_non_stream(const json& body) {
    LLMOutput out;
    if (!body.contains("content") || !body["content"].is_array()) {
        out.is_final = true;
        return out;
    }
    for (const auto& block : body["content"]) {
        const std::string ty = block.value("type", "");
        if (ty == "text") {
            out.final_answer += block.value("text", "");
        } else if (ty == "tool_use") {
            CallSpec cs;
            cs.name = block.value("name", "");
            if (block.contains("id") && block["id"].is_string()) {
                cs.tool_call_id = block["id"].get<std::string>();
            }
            if (block.contains("input") && block["input"].is_object()) {
                cs.arguments = block["input"];
            } else {
                cs.arguments = json::object();
            }
            out.tool_calls.push_back(std::move(cs));
        }
    }
    out.is_final = out.tool_calls.empty();
    return out;
}

} // namespace

AnthropicAdapter::AnthropicAdapter(const std::string& api_key, const std::string& anthropic_base,
                                   std::shared_ptr<LlmHttpTransport> transport)
    : api_key_(api_key),
      anthropic_base_(strip_trailing_slash(anthropic_base)),
      http_transport_(transport ? std::move(transport) : std::make_shared<HttplibLlmTransport>()) {}

void AnthropicAdapter::configure(const ModelConfig& config) {
    config_ = config;
    http_transport_->set_http_timeout_sec(config_.http_timeout_sec);
}

std::string AnthropicAdapter::get_model_name() const {
    return config_.model_name;
}

bool AnthropicAdapter::supports_multimodal() const {
    return true;
}

std::vector<ToolMeta> AnthropicAdapter::get_available_tools() const {
    return {};
}

json AnthropicAdapter::build_anthropic_request(const RenderedPrompt& rendered) {
    json req;
    req["model"] = config_.model_name;
    const int max_tok = std::max(config_.max_tokens, k_anthropic_min_max_tokens);
    req["max_tokens"] = max_tok;
    req["temperature"] = config_.temperature;

    std::vector<std::string> system_parts;
    for (const auto& m : rendered.messages) {
        if (!m.is_object() || m.value("role", "") != "system") {
            continue;
        }
        if (!m.contains("content")) {
            continue;
        }
        if (m["content"].is_string()) {
            system_parts.push_back(m["content"].get<std::string>());
        } else if (m["content"].is_array()) {
            for (const auto& b : m["content"]) {
                if (b.value("type", "") == "text" && b.contains("text")) {
                    system_parts.push_back(b["text"].get<std::string>());
                }
            }
        }
    }
    std::string system_joined;
    for (std::size_t i = 0; i < system_parts.size(); ++i) {
        if (i) {
            system_joined += "\n\n";
        }
        system_joined += system_parts[i];
    }
    if (!system_joined.empty()) {
        req["system"] = system_joined;
    }

    json messages = json::array();
    for (const auto& m : rendered.messages) {
        if (!m.is_object()) {
            continue;
        }
        const std::string role = m.value("role", "");
        if (role == "system") {
            continue;
        }
        if (role == "user") {
            messages.push_back(json{{"role", "user"}, {"content", user_content_from_openai_message(m)}});
        } else if (role == "assistant") {
            json content_blocks = json::array();
            if (m.contains("content") && !m["content"].is_null()) {
                if (m["content"].is_string()) {
                    const std::string t = m["content"].get<std::string>();
                    if (!t.empty()) {
                        content_blocks.push_back({{"type", "text"}, {"text", t}});
                    }
                }
            }
            if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
                for (const auto& tc : m["tool_calls"]) {
                    std::string tid = tc.value("id", "toolu_unknown");
                    std::string tname;
                    json input_obj = json::object();
                    if (tc.contains("function")) {
                        const auto& fn = tc["function"];
                        tname = fn.value("name", "");
                        const std::string args = fn.value("arguments", "{}");
                        try {
                            input_obj = json::parse(args.empty() ? "{}" : args);
                        } catch (const json::exception&) {
                            input_obj = json::object();
                        }
                    }
                    content_blocks.push_back(
                        json{{"type", "tool_use"}, {"id", tid}, {"name", tname}, {"input", input_obj}});
                }
            }
            if (content_blocks.empty()) {
                content_blocks.push_back({{"type", "text"}, {"text", ""}});
            }
            messages.push_back(json{{"role", "assistant"}, {"content", content_blocks}});
        } else if (role == "tool") {
            // TODO(tool_use_id): WP1.4 对齐 tool_call_id；缺失时用占位
            const std::string tid = m.value("tool_call_id", "toolu_unknown");
            std::string result_str;
            if (m.contains("content")) {
                if (m["content"].is_string()) {
                    result_str = m["content"].get<std::string>();
                } else {
                    result_str = m["content"].dump();
                }
            }
            messages.push_back(json{
                {"role", "user"},
                {"content",
                 json::array({{{"type", "tool_result"}, {"tool_use_id", tid}, {"content", result_str}}})}});
        }
    }
    req["messages"] = std::move(messages);

    json tools_anth = openai_tools_to_anthropic(rendered.tools_json);
    if (!tools_anth.empty()) {
        req["tools"] = std::move(tools_anth);
    }
    req["stream"] = config_.stream;
    return req;
}

std::future<LLMOutput> AnthropicAdapter::invoke(
    const LLMInput& input,
    std::function<void(std::string_view)> stream_callback) {
    return invoke_with_rendered(rendered_from_llm_input(input), std::move(stream_callback));
}

std::future<LLMOutput> AnthropicAdapter::invoke_with_rendered(
    const RenderedPrompt& rendered,
    std::function<void(std::string_view)> stream_callback) {
    return std::async(std::launch::async, [this, rendered, cb = std::move(stream_callback)]() mutable {
        return invoke_with_retries(
            [this, &rendered, &cb]() {
                http_transport_->set_http_timeout_sec(config_.http_timeout_sec);
                const std::string url = anthropic_base_ + "/v1/messages";
                json body = build_anthropic_request(rendered);
                std::map<std::string, std::string> hdrs = {
                    {"x-api-key", api_key_},
                    {"anthropic-version", k_anthropic_api_version},
                    {"Content-Type", "application/json"}};

                if (!config_.stream) {
                    json resp = http_transport_->post_llm(url, body, hdrs, "anthropic");
                    return parse_anthropic_non_stream(resp);
                }

                body["stream"] = true;
                std::string text_acc;
                struct PendingTool {
                    std::string id;
                    std::string name;
                    std::string json_frag;
                };
                std::map<int, PendingTool> tools_by_index;

                http_transport_->post_sse_cancellable(
                    url, body, hdrs,
                    [&](const std::string&, const json& j) {
                        const std::string ty = j.value("type", "");
                        if (ty == "content_block_start") {
                            const int idx = j.value("index", 0);
                            if (j.contains("content_block") && j["content_block"].is_object()) {
                                const auto& cb = j["content_block"];
                                if (cb.value("type", "") == "tool_use") {
                                    PendingTool pt;
                                    pt.id = cb.value("id", "");
                                    pt.name = cb.value("name", "");
                                    tools_by_index[idx] = std::move(pt);
                                }
                            }
                        } else if (ty == "content_block_delta") {
                            const int idx = j.value("index", 0);
                            if (!j.contains("delta") || !j["delta"].is_object()) {
                                return;
                            }
                            const auto& d = j["delta"];
                            const std::string dt = d.value("type", "");
                            if (dt == "text_delta" && d.contains("text") && d["text"].is_string()) {
                                std::string piece = d["text"].get<std::string>();
                                text_acc += piece;
                                if (cb) {
                                    cb(piece);
                                }
                            } else if (dt == "input_json_delta" && d.contains("partial_json") &&
                                       d["partial_json"].is_string()) {
                                tools_by_index[idx].json_frag += d["partial_json"].get<std::string>();
                            }
                        }
                    },
                    config_.http_timeout_sec, "anthropic", rendered.cancellation_requested);

                LLMOutput out;
                out.final_answer = text_acc;
                for (const auto& kv : tools_by_index) {
                    const PendingTool& pt = kv.second;
                    if (pt.name.empty() && pt.json_frag.empty()) {
                        continue;
                    }
                    CallSpec cs;
                    cs.name = pt.name;
                    if (!pt.id.empty()) {
                        cs.tool_call_id = pt.id;
                    }
                    try {
                        cs.arguments =
                            pt.json_frag.empty() ? json::object() : json::parse(pt.json_frag);
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
