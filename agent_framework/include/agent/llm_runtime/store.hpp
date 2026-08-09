#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/llm_runtime/types.hpp"

namespace agent_framework::llm_runtime {

enum class RuntimeStoreStatus {
    Committed,
    AlreadyExists,
    NotFound,
    RevisionConflict,
    Invalid,
    Busy,
    Error,
};

struct RuntimeStoreResult {
    RuntimeStoreStatus status{RuntimeStoreStatus::Error};
    std::uint64_t revision{0};
    std::string digest;
    std::string message;
    bool ok() const noexcept {
        return status == RuntimeStoreStatus::Committed || status == RuntimeStoreStatus::AlreadyExists;
    }
};

struct StoredInvocation {
    LLMInvocationManifest manifest;
    std::uint64_t revision{0};
    std::string updated_at;
};

class LLMRuntimeStore {
public:
    virtual ~LLMRuntimeStore() = default;
    virtual RuntimeStoreResult publish_profile(const LLMRoleProfile& profile) = 0;
    virtual std::optional<LLMRoleProfile> load_profile(
        std::string_view tenant_id, std::string_view profile_id,
        std::string_view revision) = 0;
    virtual RuntimeStoreResult publish_prompt(const PromptRevision& prompt) = 0;
    virtual std::optional<PromptRevision> load_prompt(
        std::string_view tenant_id, std::string_view prompt_id,
        std::string_view revision) = 0;
    virtual RuntimeStoreResult publish_calibration(const RoleCalibrationRecord& calibration) = 0;
    virtual std::optional<RoleCalibrationRecord> load_calibration(
        std::string_view tenant_id, std::string_view calibration_id) = 0;
    virtual RuntimeStoreResult create_invocation(const LLMInvocationManifest& manifest) = 0;
    virtual RuntimeStoreResult update_invocation(
        const LLMInvocationManifest& manifest, std::uint64_t expected_revision) = 0;
    virtual std::optional<StoredInvocation> load_invocation(
        std::string_view tenant_id, std::string_view invocation_id) = 0;
    virtual std::vector<StoredInvocation> list_recoverable(
        std::string_view tenant_id, std::size_t limit) = 0;
};

class InMemoryLLMRuntimeStore final : public LLMRuntimeStore {
public:
    RuntimeStoreResult publish_profile(const LLMRoleProfile& profile) override;
    std::optional<LLMRoleProfile> load_profile(
        std::string_view tenant_id, std::string_view profile_id,
        std::string_view revision) override;
    RuntimeStoreResult publish_prompt(const PromptRevision& prompt) override;
    std::optional<PromptRevision> load_prompt(
        std::string_view tenant_id, std::string_view prompt_id,
        std::string_view revision) override;
    RuntimeStoreResult publish_calibration(const RoleCalibrationRecord& calibration) override;
    std::optional<RoleCalibrationRecord> load_calibration(
        std::string_view tenant_id, std::string_view calibration_id) override;
    RuntimeStoreResult create_invocation(const LLMInvocationManifest& manifest) override;
    RuntimeStoreResult update_invocation(
        const LLMInvocationManifest& manifest, std::uint64_t expected_revision) override;
    std::optional<StoredInvocation> load_invocation(
        std::string_view tenant_id, std::string_view invocation_id) override;
    std::vector<StoredInvocation> list_recoverable(
        std::string_view tenant_id, std::size_t limit) override;

private:
    mutable std::mutex mutex_;
    std::map<std::string, LLMRoleProfile> profiles_;
    std::map<std::string, PromptRevision> prompts_;
    std::map<std::string, RoleCalibrationRecord> calibrations_;
    std::map<std::string, StoredInvocation> invocations_;
};

struct SQLiteLLMRuntimeStoreOptions {
    int busy_timeout_ms{5000};
    bool require_private_permissions{true};
};

class SQLiteLLMRuntimeStore final : public LLMRuntimeStore {
public:
    explicit SQLiteLLMRuntimeStore(std::string path,
                                   SQLiteLLMRuntimeStoreOptions options = {});
    ~SQLiteLLMRuntimeStore() override;
    SQLiteLLMRuntimeStore(const SQLiteLLMRuntimeStore&) = delete;
    SQLiteLLMRuntimeStore& operator=(const SQLiteLLMRuntimeStore&) = delete;

    RuntimeStoreResult publish_profile(const LLMRoleProfile& profile) override;
    std::optional<LLMRoleProfile> load_profile(
        std::string_view tenant_id, std::string_view profile_id,
        std::string_view revision) override;
    RuntimeStoreResult publish_prompt(const PromptRevision& prompt) override;
    std::optional<PromptRevision> load_prompt(
        std::string_view tenant_id, std::string_view prompt_id,
        std::string_view revision) override;
    RuntimeStoreResult publish_calibration(const RoleCalibrationRecord& calibration) override;
    std::optional<RoleCalibrationRecord> load_calibration(
        std::string_view tenant_id, std::string_view calibration_id) override;
    RuntimeStoreResult create_invocation(const LLMInvocationManifest& manifest) override;
    RuntimeStoreResult update_invocation(
        const LLMInvocationManifest& manifest, std::uint64_t expected_revision) override;
    std::optional<StoredInvocation> load_invocation(
        std::string_view tenant_id, std::string_view invocation_id) override;
    std::vector<StoredInvocation> list_recoverable(
        std::string_view tenant_id, std::size_t limit) override;

private:
    void migrate();
    std::string path_;
    SQLiteLLMRuntimeStoreOptions options_;
    void* db_{nullptr};
    std::mutex mutex_;
};

}  // namespace agent_framework::llm_runtime
