#include "agent/llm_runtime/store.hpp"

#include <algorithm>

namespace agent_framework::llm_runtime {
namespace {

std::string key(std::string_view tenant, std::string_view id,
                std::string_view revision = {}) {
    return std::string(tenant) + "\n" + std::string(id) + "\n" + std::string(revision);
}

bool terminal(InvocationState state) {
    return state == InvocationState::Succeeded || state == InvocationState::Failed ||
           state == InvocationState::Cancelled || state == InvocationState::ManualReview;
}

bool valid_transition(InvocationState from, InvocationState to) {
    if(from == to) return !terminal(from);
    if(from == InvocationState::Pending)
        return to == InvocationState::Running || to == InvocationState::Failed ||
               to == InvocationState::Cancelled;
    if(from == InvocationState::Running)
        return to == InvocationState::Succeeded || to == InvocationState::Failed ||
               to == InvocationState::Cancelled || to == InvocationState::ManualReview;
    return false;
}

template <typename T, typename Encoder, typename Decoder>
RuntimeStoreResult publish_immutable(
    std::map<std::string, T>& values, const std::string& item_key, const T& value,
    Encoder encoder, Decoder decoder) {
    const auto document = encoder(value);
    const auto digest = document.at("canonical_digest").template get<std::string>();
    const auto found = values.find(item_key);
    if(found != values.end()) {
        const auto existing_digest = encoder(found->second).at("canonical_digest").template get<std::string>();
        return {existing_digest == digest ? RuntimeStoreStatus::AlreadyExists
                                          : RuntimeStoreStatus::RevisionConflict,
                0, digest, existing_digest == digest ? "" : "immutable revision digest mismatch"};
    }
    auto decoded = decoder(document);
    if(!decoded) return {RuntimeStoreStatus::Invalid, 0, digest, "encoded document failed validation"};
    values.emplace(item_key, std::move(*decoded));
    return {RuntimeStoreStatus::Committed, 1, digest, {}};
}

}  // namespace

RuntimeStoreResult InMemoryLLMRuntimeStore::publish_profile(const LLMRoleProfile& profile) {
    std::lock_guard lock(mutex_);
    if(!validate(profile).empty()) return {RuntimeStoreStatus::Invalid, 0, {}, "invalid role profile"};
    return publish_immutable(profiles_, key(profile.metadata.identity.tenant_id, profile.profile_id, profile.revision),
        profile, [](const auto& v) { return encode(v); },
        [](const auto& v) { return decode_role_profile(v); });
}

std::optional<LLMRoleProfile> InMemoryLLMRuntimeStore::load_profile(
    std::string_view tenant_id, std::string_view profile_id, std::string_view revision) {
    std::lock_guard lock(mutex_);
    const auto found = profiles_.find(key(tenant_id, profile_id, revision));
    return found == profiles_.end() ? std::nullopt : std::optional<LLMRoleProfile>(found->second);
}

RuntimeStoreResult InMemoryLLMRuntimeStore::publish_prompt(const PromptRevision& prompt) {
    std::lock_guard lock(mutex_);
    if(!validate(prompt).empty()) return {RuntimeStoreStatus::Invalid, 0, {}, "invalid prompt revision"};
    return publish_immutable(prompts_, key(prompt.metadata.identity.tenant_id, prompt.prompt_id, prompt.revision),
        prompt, [](const auto& v) { return encode(v); },
        [](const auto& v) { return decode_prompt_revision(v); });
}

std::optional<PromptRevision> InMemoryLLMRuntimeStore::load_prompt(
    std::string_view tenant_id, std::string_view prompt_id, std::string_view revision) {
    std::lock_guard lock(mutex_);
    const auto found = prompts_.find(key(tenant_id, prompt_id, revision));
    return found == prompts_.end() ? std::nullopt : std::optional<PromptRevision>(found->second);
}

RuntimeStoreResult InMemoryLLMRuntimeStore::publish_calibration(
    const RoleCalibrationRecord& calibration) {
    std::lock_guard lock(mutex_);
    if(calibration.metadata.identity.tenant_id.empty() || calibration.calibration_id.empty())
        return {RuntimeStoreStatus::Invalid, 0, {}, "calibration tenant and id are required"};
    return publish_immutable(calibrations_, key(calibration.metadata.identity.tenant_id,
        calibration.calibration_id), calibration, [](const auto& v) { return encode(v); },
        [](const auto& v) { return decode_calibration_record(v); });
}

std::optional<RoleCalibrationRecord> InMemoryLLMRuntimeStore::load_calibration(
    std::string_view tenant_id, std::string_view calibration_id) {
    std::lock_guard lock(mutex_);
    const auto found = calibrations_.find(key(tenant_id, calibration_id));
    return found == calibrations_.end() ? std::nullopt
                                        : std::optional<RoleCalibrationRecord>(found->second);
}

RuntimeStoreResult InMemoryLLMRuntimeStore::create_invocation(
    const LLMInvocationManifest& manifest) {
    std::lock_guard lock(mutex_);
    if(manifest.state != InvocationState::Pending || !validate(manifest).empty())
        return {RuntimeStoreStatus::Invalid, 0, {}, "initial invocation must be valid and pending"};
    const auto item_key = key(manifest.metadata.identity.tenant_id, manifest.invocation_id);
    if(invocations_.count(item_key)) return {RuntimeStoreStatus::AlreadyExists, 0, {}, "invocation exists"};
    const auto document = encode(manifest);
    auto decoded = decode_invocation_manifest(document);
    if(!decoded) return {RuntimeStoreStatus::Invalid, 0, {}, "manifest encode validation failed"};
    invocations_.emplace(item_key, StoredInvocation{std::move(*decoded), 1, {}});
    return {RuntimeStoreStatus::Committed, 1,
            document.at("canonical_digest").get<std::string>(), {}};
}

RuntimeStoreResult InMemoryLLMRuntimeStore::update_invocation(
    const LLMInvocationManifest& manifest, std::uint64_t expected_revision) {
    std::lock_guard lock(mutex_);
    if(!validate(manifest).empty()) return {RuntimeStoreStatus::Invalid, 0, {}, "invalid invocation manifest"};
    const auto item_key = key(manifest.metadata.identity.tenant_id, manifest.invocation_id);
    const auto found = invocations_.find(item_key);
    if(found == invocations_.end()) return {RuntimeStoreStatus::NotFound, 0, {}, "invocation not found"};
    if(found->second.revision != expected_revision)
        return {RuntimeStoreStatus::RevisionConflict, found->second.revision, {}, "invocation revision conflict"};
    if(!valid_transition(found->second.manifest.state, manifest.state))
        return {RuntimeStoreStatus::Invalid, found->second.revision, {}, "invalid invocation state transition"};
    const auto document = encode(manifest);
    auto decoded = decode_invocation_manifest(document);
    if(!decoded) return {RuntimeStoreStatus::Invalid, found->second.revision, {}, "manifest encode validation failed"};
    found->second.manifest = std::move(*decoded);
    ++found->second.revision;
    return {RuntimeStoreStatus::Committed, found->second.revision,
            document.at("canonical_digest").get<std::string>(), {}};
}

std::optional<StoredInvocation> InMemoryLLMRuntimeStore::load_invocation(
    std::string_view tenant_id, std::string_view invocation_id) {
    std::lock_guard lock(mutex_);
    const auto found = invocations_.find(key(tenant_id, invocation_id));
    return found == invocations_.end() ? std::nullopt : std::optional<StoredInvocation>(found->second);
}

std::vector<StoredInvocation> InMemoryLLMRuntimeStore::list_recoverable(
    std::string_view tenant_id, std::size_t limit) {
    std::lock_guard lock(mutex_);
    std::vector<StoredInvocation> result;
    for(const auto& [item_key, value] : invocations_) {
        (void)item_key;
        if(value.manifest.metadata.identity.tenant_id == tenant_id && !terminal(value.manifest.state)) {
            result.push_back(value);
            if(result.size() >= limit) break;
        }
    }
    return result;
}

}  // namespace agent_framework::llm_runtime
