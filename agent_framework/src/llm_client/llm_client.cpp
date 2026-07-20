/**
 * @file llm_client.cpp
 * @brief LLMClient 注册、invoke、from_env
 */

#include <agent/llm_client/llm_client.hpp>

#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace agent_framework {

LLMClient LLMClient::from_env() {
    LLMClient client;
    const char* prov_e = std::getenv("AGENT_LLM_PROVIDER");
    const std::string prov = prov_e ? prov_e : "openai";

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
    if (const char* m = std::getenv("AGENT_LLM_MODEL")) {
        mc.model_name = m;
    }

    if (prov == "openai") {
        const char* key = std::getenv("OPENAI_API_KEY");
        if (!key || !*key) {
            throw std::runtime_error("LLMClient::from_env: OPENAI_API_KEY required for openai");
        }
        const char* bu = std::getenv("AGENT_OPENAI_BASE_URL");
        auto adapter =
            std::make_shared<OpenAIAdapter>(std::string(key), bu ? bu : "https://api.openai.com/v1");
        adapter->configure(mc);
        client.register_adapter("openai", std::move(adapter));
        client.set_default_adapter("openai");
    } else if (prov == "anthropic") {
        const char* key = std::getenv("ANTHROPIC_API_KEY");
        if (!key || !*key) {
            throw std::runtime_error("LLMClient::from_env: ANTHROPIC_API_KEY required for anthropic");
        }
        const char* ab = std::getenv("AGENT_ANTHROPIC_BASE_URL");
        auto adapter = std::make_shared<AnthropicAdapter>(std::string(key),
                                                          ab ? ab : "https://api.anthropic.com");
        adapter->configure(mc);
        client.register_adapter("anthropic", std::move(adapter));
        client.set_default_adapter("anthropic");
    } else {
        throw std::runtime_error("LLMClient::from_env: unsupported AGENT_LLM_PROVIDER=" + prov);
    }

    return client;
}

void LLMClient::set_prompt_renderer(std::shared_ptr<PromptRenderer> renderer) {
    std::lock_guard<std::mutex> lock(renderer_mutex_);
    prompt_renderer_ = std::move(renderer);
}

void LLMClient::register_adapter(const std::string& provider, std::shared_ptr<ModelAdapter> adapter) {
    std::lock_guard<std::mutex> lock(adapters_mutex_);
    adapters_[provider] = std::move(adapter);
}

void LLMClient::set_default_adapter(const std::string& provider) {
    std::lock_guard<std::mutex> lock(adapters_mutex_);
    default_provider_ = provider;
}

RenderedPrompt LLMClient::render_prompt(const LLMInput& input, const std::string& provider) {
    std::shared_ptr<PromptRenderer> renderer;
    {
        std::lock_guard<std::mutex> lock(renderer_mutex_);
        renderer = prompt_renderer_;
    }
    if (!renderer) {
        throw std::invalid_argument("LLMClient: prompt_renderer not set");
    }
    const std::string model_name = get_model_name(provider);
    return renderer->render(input, model_name);
}

std::future<LLMOutput> LLMClient::invoke(
    const LLMInput& input,
    const std::string& provider,
    std::function<void(std::string_view)> stream_callback) {
    const RenderedPrompt rendered = render_prompt(input, provider);
    if (rendered.context_budget_blocked) {
        return std::async(std::launch::deferred, []() {
            LLMOutput o;
            o.is_final = true;
            o.final_answer =
                "[context_budget] blocked: AGENT_CONTEXT_BUDGET_STRICT and combined budget still exceeded "
                "after truncation";
            return o;
        });
    }
    return invoke_with_rendered_prompt(rendered, provider, std::move(stream_callback));
}

std::future<LLMOutput> LLMClient::invoke_with_rendered_prompt(
    const RenderedPrompt& rendered,
    const std::string& provider,
    std::function<void(std::string_view)> stream_callback) {
    std::shared_ptr<ModelAdapter> adapter;
    std::string use_provider = provider;
    {
        std::lock_guard<std::mutex> lock(adapters_mutex_);
        if (use_provider.empty()) {
            use_provider = default_provider_;
        }
        const auto it = adapters_.find(use_provider);
        if (it == adapters_.end()) {
            throw std::invalid_argument("LLMClient: unknown provider: " + use_provider);
        }
        adapter = it->second;
    }
    return adapter->invoke_with_rendered(rendered, std::move(stream_callback));
}

std::future<LLMOutput> LLMClient::invoke_channels(
    const LLMInput& input, const std::string& provider,
    std::function<void(std::string_view)> answer_callback,
    std::function<void(std::string_view)> thinking_callback) {
    const RenderedPrompt rendered = render_prompt(input, provider);
    if (rendered.context_budget_blocked) {
        return std::async(std::launch::deferred, []() {
            LLMOutput output;
            output.is_final = true;
            output.final_answer =
                "[context_budget] blocked: AGENT_CONTEXT_BUDGET_STRICT and combined budget still exceeded "
                "after truncation";
            return output;
        });
    }
    return invoke_with_rendered_prompt_channels(rendered, provider, std::move(answer_callback),
                                                std::move(thinking_callback));
}

std::future<LLMOutput> LLMClient::invoke_with_rendered_prompt_channels(
    const RenderedPrompt& rendered, const std::string& provider,
    std::function<void(std::string_view)> answer_callback,
    std::function<void(std::string_view)> thinking_callback) {
    std::shared_ptr<ModelAdapter> adapter;
    std::string use_provider = provider;
    {
        std::lock_guard<std::mutex> lock(adapters_mutex_);
        if (use_provider.empty()) use_provider = default_provider_;
        const auto it = adapters_.find(use_provider);
        if (it == adapters_.end())
            throw std::invalid_argument("LLMClient: unknown provider: " + use_provider);
        adapter = it->second;
    }
    return adapter->invoke_with_rendered_channels(rendered, std::move(answer_callback),
                                                  std::move(thinking_callback));
}

void LLMClient::configure(const std::string& provider, const ModelConfig& config) {
    std::lock_guard<std::mutex> lock(adapters_mutex_);
    const auto it = adapters_.find(provider);
    if (it == adapters_.end()) {
        throw std::invalid_argument("LLMClient::configure: unknown provider: " + provider);
    }
    it->second->configure(config);
}

std::vector<std::string> LLMClient::list_providers() const {
    std::lock_guard<std::mutex> lock(adapters_mutex_);
    std::vector<std::string> names;
    names.reserve(adapters_.size());
    for (const auto& kv : adapters_) {
        names.push_back(kv.first);
    }
    return names;
}

std::string LLMClient::get_model_name(const std::string& provider) const {
    std::lock_guard<std::mutex> lock(adapters_mutex_);
    std::string use_provider = provider.empty() ? default_provider_ : provider;
    const auto it = adapters_.find(use_provider);
    if (it == adapters_.end()) {
        return {};
    }
    return it->second->get_model_name();
}

} // namespace agent_framework
