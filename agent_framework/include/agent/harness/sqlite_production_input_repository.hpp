#pragma once

#include <mutex>
#include <string>

#include "agent/harness/production_workflow_adapters.hpp"
#include "agent/tool_runtime/long_task_workflow.hpp"

namespace agent_framework::harness {

enum class ProductionInputStatus {
    Committed, AlreadyExists, RevisionConflict, Invalid, Error
};
struct ProductionInputCommit {
    ProductionInputStatus status{ProductionInputStatus::Error};
    std::uint64_t revision{0};
    std::string digest;
    std::string error;
    explicit operator bool() const noexcept {
        return status == ProductionInputStatus::Committed ||
               status == ProductionInputStatus::AlreadyExists;
    }
};

class SQLiteProductionWorkflowInputRepository final
    : public ProductionWorkflowInputRepository,
      public tool_runtime::PlanNodeInputRepository {
public:
    explicit SQLiteProductionWorkflowInputRepository(std::string path);
    ~SQLiteProductionWorkflowInputRepository() override;
    SQLiteProductionWorkflowInputRepository(const SQLiteProductionWorkflowInputRepository&) = delete;

    ProductionInputCommit put_intake(const planning::TaskIntake& value, std::uint64_t revision = 1);
    ProductionInputCommit put_memory_input(const memory_v2::workflows::MemoryWorkflowInput& value,
                                           std::string artifact_digest, std::uint64_t revision = 1);
    ProductionInputCommit put_acceptance_contract(const assurance::AcceptanceContract& value);
    ProductionInputCommit put_task_context(const contracts::ContractIdentity& identity,
                                           std::string plan_digest, nlohmann::json value,
                                           std::uint64_t revision = 1);
    ProductionInputCommit put_artifact_manifest(const contracts::ContractIdentity& identity,
                                                std::string artifact_digest, nlohmann::json value,
                                                std::uint64_t revision = 1);
    ProductionInputCommit put_acceptance_report(const assurance::AcceptanceReport& value,
                                                std::string report_digest,
                                                std::uint64_t revision = 1);
    ProductionInputCommit put_assurance_checkpoint(const assurance::AssuranceCheckpoint& value,
                                                   std::string report_digest);
    ProductionInputCommit put_impact_inventory(const remediation::ImpactInventory& value,
                                               std::uint64_t revision = 1);
    ProductionInputCommit put_evaluation_input(const JudgeWorkflowInput& value,
                                               std::uint64_t revision = 1);
    ProductionInputCommit put_plan_node_descriptor(
        const tool_runtime::PlanNodeExecutionDescriptor&, std::uint64_t revision = 1);

    std::optional<planning::TaskIntake> intake(const contracts::ContractIdentity&) override;
    std::optional<memory_v2::workflows::MemoryWorkflowInput> memory_input(
        const contracts::ContractIdentity&, std::string_view artifact_digest) override;
    std::optional<assurance::AcceptanceContract> acceptance_contract(
        const contracts::ContractIdentity&, std::string_view contract_digest) override;
    std::optional<nlohmann::json> task_context(
        const contracts::ContractIdentity&, std::string_view plan_digest) override;
    std::optional<nlohmann::json> artifact_manifest(
        const contracts::ContractIdentity&, std::string_view artifact_digest) override;
    std::optional<assurance::AcceptanceReport> acceptance_report(
        const contracts::ContractIdentity&, std::string_view report_digest) override;
    std::optional<assurance::AssuranceCheckpoint> assurance_checkpoint(
        const contracts::ContractIdentity&, std::string_view report_digest) override;
    std::optional<remediation::ImpactInventory> impact_inventory(
        const contracts::ContractIdentity&, std::string_view artifact_digest) override;
    std::optional<JudgeWorkflowInput> evaluation_input(
        const contracts::ContractIdentity&) override;
    std::optional<tool_runtime::PlanNodeExecutionDescriptor> descriptor(
        const contracts::ContractIdentity&, std::string_view plan_digest,
        std::string_view node_id) override;
private:
    ProductionInputCommit put(std::string_view kind, const contracts::ContractIdentity& identity,
                              std::string_view lookup_digest, std::uint64_t revision,
                              const nlohmann::json& document);
    std::optional<nlohmann::json> get(std::string_view kind,
        const contracts::ContractIdentity&, std::string_view lookup_digest);
    void migrate();
    void* db_{nullptr};
    std::string path_;
    std::mutex mutex_;
};

}  // namespace agent_framework::harness
