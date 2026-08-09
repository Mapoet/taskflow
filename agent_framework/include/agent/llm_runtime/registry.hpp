#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "agent/llm_runtime/store.hpp"

namespace agent_framework::llm_runtime {

class RoleProfileRegistry {
public:
    explicit RoleProfileRegistry(std::shared_ptr<LLMRuntimeStore> store);
    RuntimeStoreResult publish(const LLMRoleProfile& profile);
    std::optional<LLMRoleProfile> resolve(
        std::string_view tenant_id, std::string_view profile_id,
        std::string_view revision) const;
private:
    std::shared_ptr<LLMRuntimeStore> store_;
};

struct RenderedRolePrompt {
    std::string system_prompt;
    std::string user_prompt;
    std::string prompt_digest;
};

class PromptRegistry {
public:
    explicit PromptRegistry(std::shared_ptr<LLMRuntimeStore> store);
    RuntimeStoreResult publish(const PromptRevision& prompt);
    std::optional<PromptRevision> resolve(
        std::string_view tenant_id, std::string_view prompt_id,
        std::string_view revision) const;
    std::optional<RenderedRolePrompt> render(
        const PromptRevision& prompt,
        const std::map<std::string, std::string>& variables,
        std::string* error = nullptr) const;
private:
    std::shared_ptr<LLMRuntimeStore> store_;
};

}  // namespace agent_framework::llm_runtime
