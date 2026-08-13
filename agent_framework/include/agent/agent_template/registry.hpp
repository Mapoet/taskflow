#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/agent_template/types.hpp"

namespace agent_framework::agent_template
{

    enum class RegistryStatus
    {
        Committed,
        AlreadyExists,
        NotFound,
        RevisionConflict,
        Invalid,
        Busy,
        Error
    };
    struct RegistryResult
    {
        RegistryStatus status{RegistryStatus::Error};
        std::uint64_t revision{0};
        std::string digest;
        std::string message;
        bool ok() const noexcept
        {
            return status == RegistryStatus::Committed || status == RegistryStatus::AlreadyExists;
        }
    };
    struct StoredInvocation
    {
        AgentTemplateInvocation invocation;
        std::uint64_t store_revision{0};
        std::string updated_at;
    };

    class AgentTemplateRegistry
    {
    public:
        virtual ~AgentTemplateRegistry() = default;
        virtual RegistryResult publish(const AgentTemplate &value) = 0;
        virtual std::optional<AgentTemplate> load(std::string_view tenant_id,
                                                  std::string_view template_id,
                                                  std::uint64_t revision) = 0;
        virtual std::optional<AgentTemplate> latest(std::string_view tenant_id,
                                                    std::string_view template_id) = 0;
        virtual RegistryResult create_invocation(const AgentTemplateInvocation &value) = 0;
        virtual RegistryResult update_invocation(const AgentTemplateInvocation &value,
                                                 std::uint64_t expected_store_revision) = 0;
        virtual std::optional<StoredInvocation> load_invocation(
            std::string_view tenant_id, std::string_view invocation_id) = 0;
    };

    struct SQLiteRegistryOptions
    {
        int busy_timeout_ms{5000};
        bool require_private_permissions{true};
    };

    class SQLiteAgentTemplateRegistry final : public AgentTemplateRegistry
    {
    public:
        explicit SQLiteAgentTemplateRegistry(std::string path,
                                             SQLiteRegistryOptions options = {});
        ~SQLiteAgentTemplateRegistry() override;
        SQLiteAgentTemplateRegistry(const SQLiteAgentTemplateRegistry &) = delete;
        SQLiteAgentTemplateRegistry &operator=(const SQLiteAgentTemplateRegistry &) = delete;

        RegistryResult publish(const AgentTemplate &value) override;
        std::optional<AgentTemplate> load(std::string_view tenant_id,
                                          std::string_view template_id,
                                          std::uint64_t revision) override;
        std::optional<AgentTemplate> latest(std::string_view tenant_id,
                                            std::string_view template_id) override;
        RegistryResult create_invocation(const AgentTemplateInvocation &value) override;
        RegistryResult update_invocation(const AgentTemplateInvocation &value,
                                         std::uint64_t expected_store_revision) override;
        std::optional<StoredInvocation> load_invocation(
            std::string_view tenant_id, std::string_view invocation_id) override;

    private:
        void migrate();
        std::string path_;
        SQLiteRegistryOptions options_;
        void *db_{nullptr};
        std::mutex mutex_;
    };

} // namespace agent_framework::agent_template
