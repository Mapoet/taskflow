#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "agent/llm_runtime/types.hpp"

namespace agent_framework::llm_runtime {

enum class OutputGateStatus { Accepted, ParseFailed, SchemaFailed, NotRequired };

struct OutputGateResult {
    OutputGateStatus status{OutputGateStatus::ParseFailed};
    std::optional<nlohmann::json> value;
    nlohmann::json validation_error = nlohmann::json::object();
    std::string code;
    std::string message;
    bool accepted() const noexcept {
        return status == OutputGateStatus::Accepted || status == OutputGateStatus::NotRequired;
    }
};

class StructuredOutputGate {
public:
    bool validate_input(const PromptRevision& prompt,
                        const nlohmann::json& input,
                        nlohmann::json* error = nullptr) const;
    OutputGateResult validate_output(const PromptRevision& prompt,
                                     const std::string& text) const;
    std::string repair_instruction(const PromptRevision& prompt,
                                   const OutputGateResult& failure) const;
};

}  // namespace agent_framework::llm_runtime
