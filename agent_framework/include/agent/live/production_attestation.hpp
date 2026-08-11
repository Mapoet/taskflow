#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/live/role_certification.hpp"

namespace agent_framework::live {

struct ProductionCellAttestation {
    std::string tenant_id;
    std::string attestation_id;
    std::string environment_digest;
    std::string matrix_digest;
    std::string cell_id;
    std::string spec_digest;
    std::string invocation_id;
    std::string invocation_manifest_digest;
    std::string result_digest;
    std::vector<std::string> evidence_digests;
    std::vector<std::string> oracle_digests;
    std::string trace_id;
    std::string source;
    std::string source_signature_digest;
    std::string recorded_at;
};

class ProductionAttestationStore {
public:
    virtual ~ProductionAttestationStore() = default;
    virtual bool append(const ProductionCellAttestation& value, std::string* error = nullptr) = 0;
    virtual std::optional<ProductionCellAttestation> find(
        std::string_view tenant_id, std::string_view invocation_id,
        std::string* error = nullptr) = 0;
};

class SQLiteProductionAttestationStore final : public ProductionAttestationStore {
public:
    explicit SQLiteProductionAttestationStore(std::string path, int busy_timeout_ms = 3000);
    ~SQLiteProductionAttestationStore() override;
    SQLiteProductionAttestationStore(const SQLiteProductionAttestationStore&) = delete;
    SQLiteProductionAttestationStore& operator=(const SQLiteProductionAttestationStore&) = delete;
    bool append(const ProductionCellAttestation& value, std::string* error = nullptr) override;
    std::optional<ProductionCellAttestation> find(
        std::string_view tenant_id, std::string_view invocation_id,
        std::string* error = nullptr) override;
private:
    void* db_{nullptr};
    std::mutex mutex_;
};

std::string production_cell_result_digest(const LiveCellResult& result);

class StoreBackedCellEvidenceVerifier {
public:
    StoreBackedCellEvidenceVerifier(ProductionAttestationStore& store,
                                    std::string expected_matrix_digest);
    bool verify(const LiveEnvironmentProfile& environment, const LiveCellSpec& spec,
                const LiveCellResult& result, std::string* error = nullptr);
private:
    ProductionAttestationStore& store_;
    std::string expected_matrix_digest_;
};

}  // namespace agent_framework::live
