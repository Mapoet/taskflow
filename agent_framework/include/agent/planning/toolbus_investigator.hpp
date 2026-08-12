#pragma once

#include <memory>
#include <string>
#include <vector>

#include "agent/planning/cognition_workflow.hpp"
#include "agent/toolbus/toolbus.hpp"

namespace agent_framework::planning {

struct ToolBusInvestigationStep {
    std::string tool_name;
    nlohmann::json arguments = nlohmann::json::object();
    std::string locator;
    std::vector<std::string> supported_claims;
    std::vector<std::string> contradicted_claims;
    std::string trust_class{"repository"};
    std::string freshness_deadline;
};

struct ToolBusInvestigatorOptions {
    std::string investigator_id;
    bool external{false};
    std::vector<std::string> required_capabilities;
    std::vector<ToolBusInvestigationStep> steps;
    std::size_t maximum_result_bytes{1024 * 1024};
    std::function<std::string()> now;
};

// Production adapter: the plan is fixed by trusted deployment configuration;
// LLM output can select the investigator but cannot inject arbitrary tool calls.
class ToolBusInvestigator final : public Investigator {
public:
    ToolBusInvestigator(std::shared_ptr<ToolBus> tools, ToolBusInvestigatorOptions options);
    std::string id() const override { return options_.investigator_id; }
    bool external() const noexcept override { return options_.external; }
    bool read_only() const noexcept override { return true; }
    std::vector<std::string> required_capabilities() const override {
        return options_.required_capabilities;
    }
    std::vector<EvidenceRecord> investigate(const InvestigationRequest& request,
                                             std::string* error) override;
private:
    std::shared_ptr<ToolBus> tools_;
    ToolBusInvestigatorOptions options_;
};

}  // namespace agent_framework::planning
