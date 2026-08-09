#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "agent/llm_client/llm_client.hpp"
#include "agent/llm_runtime/registry.hpp"
#include "agent/llm_runtime/router.hpp"
#include "agent/llm_runtime/structured_output.hpp"

namespace agent_framework {
class AuditSink;
namespace telemetry { class TelemetryRuntime; }
}

namespace agent_framework::llm_runtime {

struct RoleRuntimeOptions {
    bool require_approved_calibration{true};
    bool manual_review_uncertain_running{true};
    std::function<std::string()> now;
    std::function<std::string()> next_id;
};

class RoleRuntime {
public:
    RoleRuntime(std::shared_ptr<LLMClient> client,
                std::shared_ptr<LLMRuntimeStore> store,
                std::shared_ptr<ModelRouter> router,
                std::shared_ptr<telemetry::TelemetryRuntime> telemetry = nullptr,
                std::shared_ptr<AuditSink> audit = nullptr,
                RoleRuntimeOptions options = {});

    RoleProfileRegistry& profiles() noexcept { return profiles_; }
    PromptRegistry& prompts() noexcept { return prompts_; }
    std::shared_ptr<LLMRuntimeStore> store() const noexcept { return store_; }
    std::shared_ptr<ModelRouter> router() const noexcept { return router_; }

    RoleInvocationResult invoke(
        RoleInvocationRequest request,
        std::function<void(std::string_view)> answer_callback = nullptr,
        std::function<void(std::string_view)> thinking_summary_callback = nullptr);

    std::vector<StoredInvocation> reconcile_recoverable(
        std::string_view tenant_id, std::size_t limit = 100);

private:
    std::shared_ptr<LLMClient> client_;
    std::shared_ptr<LLMRuntimeStore> store_;
    std::shared_ptr<ModelRouter> router_;
    std::shared_ptr<telemetry::TelemetryRuntime> telemetry_;
    std::shared_ptr<AuditSink> audit_;
    RoleRuntimeOptions options_;
    RoleProfileRegistry profiles_;
    PromptRegistry prompts_;
    StructuredOutputGate output_gate_;
};

}  // namespace agent_framework::llm_runtime
