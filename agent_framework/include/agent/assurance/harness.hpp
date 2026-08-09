#pragma once

#include "agent/assurance/arbiter.hpp"
#include "agent/assurance/registry.hpp"
#include "agent/memory_v2/view_profiles.hpp"

namespace agent_framework::assurance {

struct HarnessResult {
    AcceptanceReport report;
    std::vector<std::string> verifier_errors;
    bool fail_closed{false};
};

class AssuranceHarness {
public:
    AssuranceHarness(memory_v2::MemoryViewEngine& views, VerifierRegistry& verifiers,
                     AcceptanceArbiter arbiter = {});
    HarnessResult verify(const AcceptanceContract& contract,
                         const memory_v2::MemoryScope& subject,
                         const nlohmann::json& artifact_manifest,
                         std::string_view now = {}, std::string_view deadline = {});
private:
    memory_v2::MemoryViewEngine& views_;
    VerifierRegistry& verifiers_;
    AcceptanceArbiter arbiter_;
};

}  // namespace agent_framework::assurance
