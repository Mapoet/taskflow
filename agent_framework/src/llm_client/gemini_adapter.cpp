/**
 * @file gemini_adapter.cpp
 * @brief Gemini 适配器占位（WP1.1 未实现）
 */

#include "agent/llm_client.hpp"

#include <future>
#include <stdexcept>
#include <utility>

namespace agent_framework {

GeminiAdapter::GeminiAdapter(const std::string& api_key) : api_key_(api_key) {}

std::future<LLMOutput> GeminiAdapter::invoke(const LLMInput& /*input*/,
                                             std::function<void(std::string_view)> /*cb*/) {
    return std::async(std::launch::async,
                       []() -> LLMOutput { throw std::runtime_error("GeminiAdapter: not implemented"); });
}

std::future<LLMOutput> GeminiAdapter::invoke_with_rendered(
    const RenderedPrompt& /*rendered*/,
    std::function<void(std::string_view)> /*cb*/) {
    return std::async(std::launch::async,
                       []() -> LLMOutput { throw std::runtime_error("GeminiAdapter: not implemented"); });
}

std::vector<ToolMeta> GeminiAdapter::get_available_tools() const {
    return {};
}

void GeminiAdapter::configure(const ModelConfig& config) {
    config_ = config;
}

std::string GeminiAdapter::get_model_name() const {
    return config_.model_name;
}

bool GeminiAdapter::supports_multimodal() const {
    return false;
}

json GeminiAdapter::build_gemini_request(const RenderedPrompt&) {
    throw std::logic_error("GeminiAdapter::build_gemini_request unused");
}

} // namespace agent_framework
