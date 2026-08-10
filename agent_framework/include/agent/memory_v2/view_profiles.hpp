#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "agent/memory_v2/types.hpp"

namespace agent_framework::memory_v2 {

enum class MemoryViewMode {
    Intake,
    Investigation,
    Planning,
    Execution,
    Verification,
    Replan,
    Resume,
    Handoff,
    Evaluation
};

struct ViewBudget {
    std::uint64_t bytes{256 * 1024};
    std::uint64_t tokens{32 * 1024};
};

std::string_view to_string(MemoryViewMode mode) noexcept;
std::optional<MemoryViewMode> memory_view_mode(std::string_view value) noexcept;
MemoryViewSpec make_view_spec(MemoryViewMode mode,
                              contracts::ContractMetadata metadata,
                              MemoryScope subject,
                              ViewBudget budget = {});

class MemoryViewRouter {
public:
    std::optional<MemoryViewMode> route(MemoryViewMode current,
                                        std::string_view workflow_event) const noexcept;
};

}  // namespace agent_framework::memory_v2
