#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "agent/memory_v2/provider.hpp"

namespace agent_framework::memory_v2 {

struct MemoryView {
    MemorySnapshot snapshot;
    MemoryViewManifest manifest;
    std::vector<MemoryRecord> records;
    bool fail_closed{false};
    std::string error;
};

class MemoryViewEngine {
public:
    explicit MemoryViewEngine(MemoryProviderRegistry& providers) : providers_(providers) {}
    MemoryView build(const MemoryViewSpec& spec, std::string_view now = {});
private:
    MemoryProviderRegistry& providers_;
};

}  // namespace agent_framework::memory_v2
