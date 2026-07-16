/** @file multimodal_agent.cpp @brief Deterministic A2A multimodal message example. */

#include <agent/agent/user_input_preprocessor.hpp>
#include <agent/core/types.hpp>

#include <chrono>
#include <iostream>

using namespace agent_framework;

int main() {
    AgentMessage message;
    message.role = AgentMessage::Role::USER;
    message.message_id = "multimodal-example-1";
    message.timestamp = std::chrono::system_clock::now();
    message.parts = {
        AgentPart{AgentPart::Type::TEXT, std::string("Summarize observation"), std::nullopt, std::nullopt},
        AgentPart{AgentPart::Type::DATA, std::nullopt, std::nullopt,
                  nlohmann::json{{"instrument", "GNSS-RO"}, {"occultations", 42}}}
    };

    const auto text = concat_user_text_from_message(message);
    const auto wire = message.to_json();
    if(text != "Summarize observation" || wire["parts"].size() != 2) return 1;
    std::cout << wire.dump(2) << '\n';
    return 0;
}
