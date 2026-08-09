#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/llm_runtime/types.hpp"

namespace agent_framework::llm_runtime {

class ModelRouter {
public:
    bool register_candidate(ProviderModel candidate);
    bool set_available(std::string_view candidate_id, bool available);
    std::optional<ProviderModel> find(std::string_view candidate_id) const;
    std::vector<ProviderModel> candidates() const;
    ModelRouteDecision route(
        const LLMRoleProfile& profile, const RoleInvocationRequest& request,
        const std::vector<std::string>& excluded_candidate_ids = {}) const;
private:
    mutable std::mutex mutex_;
    std::map<std::string, ProviderModel> candidates_;
};

}  // namespace agent_framework::llm_runtime
