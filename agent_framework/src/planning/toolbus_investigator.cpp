#include "agent/planning/toolbus_investigator.hpp"

#include <stdexcept>

#include "agent/contracts/contract.hpp"
#include "agent/observability/audit.hpp"

namespace agent_framework::planning {

ToolBusInvestigator::ToolBusInvestigator(std::shared_ptr<ToolBus> tools,
                                         ToolBusInvestigatorOptions options)
    : tools_(std::move(tools)), options_(std::move(options)) {
    if(!tools_ || options_.investigator_id.empty() || options_.steps.empty() ||
       options_.maximum_result_bytes == 0)
        throw std::invalid_argument("ToolBus investigator requires bus, id, steps and result budget");
    for(const auto& step : options_.steps) {
        if(step.tool_name.empty() || step.locator.empty())
            throw std::invalid_argument("investigation tool and locator are required");
        const auto meta = tools_->get_tool_meta(step.tool_name);
        if(meta.name.empty() || meta.side_effect != ToolSideEffect::ReadOnly)
            throw std::invalid_argument("production investigator accepts only registered read-only tools");
    }
}

std::vector<EvidenceRecord> ToolBusInvestigator::investigate(
    const InvestigationRequest& request, std::string* error) {
    if(request.remaining_tool_calls < options_.steps.size()) {
        if(error) *error = "investigator tool budget is smaller than configured plan";
        return {};
    }
    std::vector<EvidenceRecord> records;
    records.reserve(options_.steps.size());
    for(std::size_t index = 0; index < options_.steps.size(); ++index) {
        if(request.intake.metadata.identity.tenant_id.empty()) {
            if(error) *error = "investigation scope is required";
            return {};
        }
        const auto& step = options_.steps[index];
        ToolCallControl control;
        const auto result = tools_->call_tool(step.tool_name, step.arguments, control).get();
        if(result.is_object() && result.contains("error")) {
            if(error) *error = "tool failed: " + step.tool_name;
            return {};
        }
        const auto document = contracts::canonical_json(result);
        if(document.size() > options_.maximum_result_bytes) {
            if(error) *error = "tool result exceeds evidence size budget";
            return {};
        }
        EvidenceRecord record;
        record.evidence_id = options_.investigator_id + ":" + std::to_string(index) + ":" +
            contracts::canonical_digest(result).value_or("");
        record.origin_kind = options_.external ? "external" : "repository";
        record.locator = step.locator;
        record.content_digest = contracts::canonical_digest(result).value_or("");
        record.collected_at = options_.now ? options_.now() : audit_timestamp_now();
        record.trust_class = step.trust_class;
        record.freshness_deadline = step.freshness_deadline;
        record.supported_claims = step.supported_claims;
        record.contradicted_claims = step.contradicted_claims;
        record.instruction_authority = false;
        records.push_back(std::move(record));
    }
    return records;
}

}  // namespace agent_framework::planning
