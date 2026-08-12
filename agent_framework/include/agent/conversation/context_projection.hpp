#pragma once
#include "agent/conversation/store.hpp"
namespace agent_framework::conversation {
class ContextProjector {
public:
    static std::optional<ContextProjectionManifest> build(
        ContextProjectionManifest manifest,std::string* error=nullptr);
    static bool validate_boundary(const ContextProjectionManifest& before,
                                  const CompactBoundaryRecord& boundary,
                                  std::string* error=nullptr);
};
}
