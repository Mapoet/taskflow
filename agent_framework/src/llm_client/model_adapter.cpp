/**
 * @file model_adapter.cpp
 * @brief ModelAdapter 基类默认实现与重试
 */

#include <agent/llm_client/llm_client.hpp>
#include <agent/llm_client/llm_retry.hpp>

#include <chrono>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

namespace agent_framework {

namespace {

bool should_retry_status(int status_code) {
    return status_code == 0 || status_code == 408 || status_code == 429 || status_code == 502 ||
           status_code == 503 || status_code == 504;
}

bool looks_like_transport_failure(const std::runtime_error& e) {
    const char* msg = e.what();
    return std::strstr(msg, "failed") != nullptr || std::strstr(msg, "error code") != nullptr ||
           std::strstr(msg, "invalid client") != nullptr || std::strstr(msg, "transport") != nullptr;
}

} // namespace

json ModelAdapter::send_request(const std::string&, const json&) {
    throw std::logic_error("ModelAdapter::send_request unused");
}

json ModelAdapter::parse_response(const std::string&) {
    throw std::logic_error("ModelAdapter::parse_response unused");
}

LLMOutput invoke_with_retries(std::function<LLMOutput()> fn, const ModelConfig& config) {
    const int max_attempts = std::max(1, config.max_retries + 1);
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> jitter(0, 250);

    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        try {
            return fn();
        } catch (const llm_http_error& e) {
            if (attempt + 1 >= max_attempts || !should_retry_status(e.status_code)) {
                throw;
            }
            int delay_ms = 500 * (1 << attempt) + jitter(rng);
            if (e.retry_after_sec.has_value() && *e.retry_after_sec > 0) {
                delay_ms = *e.retry_after_sec * 1000;
            }
            if (delay_ms > 8000) {
                delay_ms = 8000;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        } catch (const std::runtime_error& e) {
            if (attempt + 1 >= max_attempts || !looks_like_transport_failure(e)) {
                throw;
            }
            int delay_ms = 500 * (1 << attempt) + jitter(rng);
            if (delay_ms > 8000) {
                delay_ms = 8000;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }
    throw std::logic_error("invoke_with_retries: unreachable");
}

} // namespace agent_framework
