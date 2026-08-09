#include "agent/llm_runtime/structured_output.hpp"

#include "agent/toolbus/schema_validate.hpp"

namespace agent_framework::llm_runtime {

bool StructuredOutputGate::validate_input(
    const PromptRevision& prompt, const nlohmann::json& input,
    nlohmann::json* error) const {
    nlohmann::json local;
    const bool valid = validate_json_instance(prompt.input_schema, input, local);
    if(error) *error = std::move(local);
    return valid;
}

OutputGateResult StructuredOutputGate::validate_output(
    const PromptRevision& prompt, const std::string& text) const {
    if(!prompt.structured_output_required)
        return {OutputGateStatus::NotRequired, std::nullopt, {}, "not_required", {}};
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(text);
    } catch(const nlohmann::json::exception& error) {
        return {OutputGateStatus::ParseFailed, std::nullopt,
                {{"exception", error.what()}}, "output_json_parse_failed",
                "model output is not strict JSON"};
    }
    nlohmann::json validation_error;
    if(!validate_json_instance(prompt.output_schema, parsed, validation_error))
        return {OutputGateStatus::SchemaFailed, std::nullopt, std::move(validation_error),
                "output_schema_failed", "model output does not satisfy the pinned schema"};
    return {OutputGateStatus::Accepted, std::move(parsed), {}, "accepted", {}};
}

std::string StructuredOutputGate::repair_instruction(
    const PromptRevision& prompt, const OutputGateResult& failure) const {
    return "Your prior response failed the pinned structured-output contract. "
           "Return only one strict JSON value matching this schema; do not use Markdown fences.\n"
           "Schema: " + prompt.output_schema.dump() + "\nValidation failure: " +
           failure.validation_error.dump();
}

}  // namespace agent_framework::llm_runtime
