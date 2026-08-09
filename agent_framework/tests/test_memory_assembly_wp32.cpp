#include <agent/memory/memory_assembly.hpp>
#include <cassert>
#include <iostream>
using namespace agent_framework;
int main() {
    MemoryAssemblyPolicy policy; policy.hard_limit_bytes = 20; policy.default_slot_quota_bytes = 12;
    auto result = assemble_memory({
        {MemorySlotKind::Retrieval, "r", 1, 0, "retrieval-payload"},
        {MemorySlotKind::System, "s", 10, 0, "system"},
        {MemorySlotKind::Task, "t", 9, 0, "task"}}, policy);
    assert(result.text.find("system") != std::string::npos && result.text.find("task") != std::string::npos);
    assert(result.report.input_bytes == 27 && result.report.output_bytes <= 20);
    assert(!result.report.decisions.empty());
    for(const auto& decision : result.report.decisions) assert(decision.dump().find("payload") == std::string::npos);
    std::cout << "test_memory_assembly_wp32: ok\n";
}
