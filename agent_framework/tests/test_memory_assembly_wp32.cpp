#include <agent/memory/memory_assembly.hpp>

#include <stdexcept>

using namespace agent_framework;

namespace {
void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}

MemorySlot slot(MemorySlotKind kind, std::string id, int priority, std::string text,
                json provenance = json::object()) {
    return {kind, std::move(id), priority, 0, std::move(text), std::move(provenance)};
}
}

int main() {
    MemoryAssemblyInput input;
    input.system.push_back(slot(MemorySlotKind::System, "secret-system-id", 1000, "system"));
    input.task.push_back(slot(MemorySlotKind::Task, "task-id", 900, "task"));
    input.working.push_back(slot(MemorySlotKind::Working, "working-id", 500, "working-memory"));
    input.retrieval.push_back(slot(MemorySlotKind::Retrieval, "citation-id", 600,
                                   "[source:doc-1] evidence",
                                   json{{"citation_id", "doc-1"}}));
    input.tool.push_back(slot(MemorySlotKind::Tool, "tool-id", 100,
                              "SECRET_TOOL_PAYLOAD_THAT_MUST_BE_EVICTED"));
    input.skill.push_back(slot(MemorySlotKind::Skill, "skill-id", 400, "skill"));

    MemoryAssemblyPolicy policy;
    policy.soft_limit_bytes = 56;
    policy.hard_limit_bytes = 64;
    policy.slot_quota_bytes[MemorySlotKind::Tool] = 16;
    policy.minimum_retained_bytes[MemorySlotKind::System] = 1;
    policy.minimum_retained_bytes[MemorySlotKind::Task] = 1;
    policy.minimum_retained_bytes[MemorySlotKind::Tool] = 8;
    policy.revision = "wp32-contract-v1";
    policy.token_estimator = [](std::string_view text) { return text.size(); };

    const auto first = assemble_memory(input, policy);
    const auto second = assemble_memory(input, policy);
    require(first.text == second.text, "assembly is not deterministic");
    require(first.report.to_json() == second.report.to_json(), "assembly report is not deterministic");
    require(first.report.input_slots == 6, "six input kinds were not assembled");
    require(first.report.output_bytes <= policy.hard_limit_bytes, "hard budget exceeded");
    require(first.text.find("system") != std::string::npos, "system slot lost");
    require(first.text.find("task") != std::string::npos, "task slot lost");
    require(first.text.find("[source:doc-1]") != std::string::npos, "citation lost");
    require(first.text.find("SECRET_TOOL_PAYLOAD") == std::string::npos,
            "tool result was not evicted before protected slots");
    require(first.report.policy_revision == "wp32-contract-v1", "policy revision missing");
    require(first.report.output_tokens == first.report.output_bytes,
            "configured token estimator was not used");

    const auto report = first.report.to_json().dump();
    require(report.find("secret-system-id") == std::string::npos, "raw source id leaked");
    require(report.find("SECRET_TOOL_PAYLOAD") == std::string::npos, "raw text leaked");
    for(const auto& decision : first.report.decisions) {
        require(decision.contains("source_id_digest"), "decision lacks source digest");
        require(!decision.contains("source_id"), "decision contains raw source id");
    }

    MemoryAssemblyPolicy impossible;
    impossible.hard_limit_bytes = 5;
    impossible.minimum_retained_bytes[MemorySlotKind::System] = 1;
    impossible.minimum_retained_bytes[MemorySlotKind::Task] = 1;
    const auto bounded = assemble_memory(input, impossible);
    require(bounded.report.output_bytes <= 5, "protected slots bypassed hard limit");
}
