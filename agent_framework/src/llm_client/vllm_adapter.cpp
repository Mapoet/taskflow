/**
 * @file vllm_adapter.cpp
 * @brief vLLM 适配器占位（WP1.1 未实现）
 */

#include "agent/llm_client.hpp"

#include <future>
#include <stdexcept>
#include <utility>

namespace agent_framework {

vLLMAdapter::vLLMAdapter(const std::string& endpoint) : endpoint_(endpoint) {}

std::future<LLMOutput> vLLMAdapter::invoke(const LLMInput& /*input*/,
                                           std::function<void(std::string_view)> /*cb*/) {
    return std::async(std::launch::async,
                       []() -> LLMOutput { throw std::runtime_error("vLLMAdapter: not implemented"); });
}

std::future<LLMOutput> vLLMAdapter::invoke_with_rendered(
    const RenderedPrompt& /*rendered*/,
    std::function<void(std::string_view)> /*cb*/) {
    return std::async(std::launch::async,
                       []() -> LLMOutput { throw std::runtime_error("vLLMAdapter: not implemented"); });
}

std::vector<ToolMeta> vLLMAdapter::get_available_tools() const {
    return {};
}

void vLLMAdapter::configure(const ModelConfig& config) {
    config_ = config;
}

std::string vLLMAdapter::get_model_name() const {
    return config_.model_name;
}

bool vLLMAdapter::supports_multimodal() const {
    return false;
}

json vLLMAdapter::build_vllm_request(const RenderedPrompt&) {
    throw std::logic_error("vLLMAdapter::build_vllm_request unused");
}

} // namespace agent_framework
