#include "agent/llm_runtime/registry.hpp"

#include <stdexcept>

namespace agent_framework::llm_runtime {
namespace {

std::optional<std::string> render_template(
    std::string value, const std::map<std::string, std::string>& variables,
    std::string* error) {
    std::size_t position = 0;
    while((position = value.find("{{", position)) != std::string::npos) {
        const auto end = value.find("}}", position + 2);
        if(end == std::string::npos) {
            if(error) *error = "unterminated prompt variable";
            return std::nullopt;
        }
        const std::string key = value.substr(position + 2, end - position - 2);
        if(key.empty()) {
            if(error) *error = "empty prompt variable";
            return std::nullopt;
        }
        const auto found = variables.find(key);
        if(found == variables.end()) {
            if(error) *error = "missing prompt variable: " + key;
            return std::nullopt;
        }
        value.replace(position, end + 2 - position, found->second);
        position += found->second.size();
    }
    return value;
}

}  // namespace

RoleProfileRegistry::RoleProfileRegistry(std::shared_ptr<LLMRuntimeStore> store)
    : store_(std::move(store)) {
    if(!store_) throw std::invalid_argument("role profile registry store is required");
}

RuntimeStoreResult RoleProfileRegistry::publish(const LLMRoleProfile& profile) {
    return store_->publish_profile(profile);
}

std::optional<LLMRoleProfile> RoleProfileRegistry::resolve(
    std::string_view tenant_id, std::string_view profile_id,
    std::string_view revision) const {
    if(tenant_id.empty() || profile_id.empty() || revision.empty()) return std::nullopt;
    return store_->load_profile(tenant_id, profile_id, revision);
}

PromptRegistry::PromptRegistry(std::shared_ptr<LLMRuntimeStore> store)
    : store_(std::move(store)) {
    if(!store_) throw std::invalid_argument("prompt registry store is required");
}

RuntimeStoreResult PromptRegistry::publish(const PromptRevision& prompt) {
    return store_->publish_prompt(prompt);
}

std::optional<PromptRevision> PromptRegistry::resolve(
    std::string_view tenant_id, std::string_view prompt_id,
    std::string_view revision) const {
    if(tenant_id.empty() || prompt_id.empty() || revision.empty()) return std::nullopt;
    return store_->load_prompt(tenant_id, prompt_id, revision);
}

std::optional<RenderedRolePrompt> PromptRegistry::render(
    const PromptRevision& prompt,
    const std::map<std::string, std::string>& variables,
    std::string* error) const {
    auto system = render_template(prompt.system_template, variables, error);
    if(!system) return std::nullopt;
    auto user = render_template(prompt.user_template, variables, error);
    if(!user) return std::nullopt;
    const auto document = encode(prompt);
    return RenderedRolePrompt{std::move(*system), std::move(*user),
                              document.at("canonical_digest").get<std::string>()};
}

}  // namespace agent_framework::llm_runtime
