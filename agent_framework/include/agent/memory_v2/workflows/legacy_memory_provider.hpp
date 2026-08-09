#pragma once

#include <memory>
#include <string>

#include "agent/memory/memory.hpp"
#include "agent/memory_v2/provider.hpp"

namespace agent_framework::memory_v2::workflows {

struct LegacyMemoryProviderConfig {
    std::string provider_id{"legacy-memory-v1"};
    MemoryScope fixed_scope;
    std::string session_id;
    std::string summary_query;
    int max_messages{20};
    int top_k_summaries{10};
};

// Dual-read adapter only. Legacy content is projected as observed/candidate data and is
// never promoted or persisted into Memory v2 by this provider.
class LegacyMemoryProvider final : public memory_v2::MemoryProvider {
public:
    LegacyMemoryProvider(std::shared_ptr<agent_framework::MemoryStore> legacy,
                         LegacyMemoryProviderConfig config);
    std::string id() const override;
    ProviderResult fetch(const MemoryQuery& query) override;

private:
    std::shared_ptr<agent_framework::MemoryStore> legacy_;
    LegacyMemoryProviderConfig config_;
};

}  // namespace agent_framework::memory_v2::workflows
