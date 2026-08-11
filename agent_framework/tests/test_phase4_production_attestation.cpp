#include <cassert>
#include <filesystem>
#include <string>

#include <unistd.h>

#include "agent/live/production_attestation.hpp"
#include "phase4_live_test_support.hpp"

int main() {
    using namespace agent_framework::live;
    using namespace phase4_live_test;
    auto environment_value = environment("task-r6l-attestation");
    auto matrix_value = matrix(environment_value, false);
    const auto matrix_digest = role_live_matrix_digest(matrix_value);
    const auto path = std::filesystem::temp_directory_path() /
        ("phase4-production-attestation-" + std::to_string(getpid()) + ".sqlite");
    std::filesystem::remove(path);

    DeterministicExecutor executor;
    {
        SQLiteProductionAttestationStore attestations(path.string());
        for(const auto& spec : matrix_value.cells) {
            auto result = executor.execute({environment_value, matrix_value, spec, {}});
            ProductionCellAttestation value;
            value.tenant_id = environment_value.metadata.identity.tenant_id;
            value.attestation_id = "att-" + spec.cell_id;
            value.environment_digest = role_environment_digest(environment_value);
            value.matrix_digest = matrix_digest;
            value.cell_id = spec.cell_id;
            value.spec_digest = live_cell_spec_digest(spec);
            value.invocation_id = result.invocation_id;
            value.invocation_manifest_digest = result.invocation_manifest_digest;
            value.result_digest = production_cell_result_digest(result);
            value.evidence_digests = result.evidence_digests;
            value.oracle_digests = result.oracle_digests;
            value.trace_id = "trace-r6l";
            value.source = "immutable-audit-store";
            value.source_signature_digest = "sha256:source-signature";
            value.recorded_at = "2026-08-10T01:00:02Z";
            std::string error;
            assert(attestations.append(value, &error));
            assert(!attestations.append(value, &error));  // immutable replay/conflict
        }
    }

    // A new process-equivalent Store object reconstructs evidence from SQLite.
    SQLiteProductionAttestationStore restarted(path.string());
    StoreBackedCellEvidenceVerifier verifier(restarted, matrix_digest);
    InMemoryRoleCertificationStore reports;
    RoleLiveCertificationWorkflow workflow(reports, executor);
    auto workflow_options = options("workflow-r6l-attested");
    workflow_options.cell_evidence_verifier = [&](const auto& e, const auto& spec,
                                                   const auto& result, auto* error) {
        return verifier.verify(e, spec, result, error);
    };
    const auto certified = workflow.run(environment_value, matrix_value, workflow_options);
    assert(certified.state == RoleCertificationState::Certified);
    assert(certified.report && certified.report->executed && certified.report->blockers.empty());

    auto tampered_result = executor.execute(
        {environment_value, matrix_value, matrix_value.cells.front(), {}});
    tampered_result.evidence_digests = {"sha256:forged"};
    std::string error;
    assert(!verifier.verify(environment_value, matrix_value.cells.front(), tampered_result, &error));
    assert(error == "attestation binding mismatch");

    auto other_environment = environment_value;
    other_environment.git_revision = "git:other";
    error.clear();
    assert(!verifier.verify(other_environment, matrix_value.cells.front(),
                            executor.execute({environment_value, matrix_value,
                                              matrix_value.cells.front(), {}}), &error));
    assert(error == "attestation binding mismatch");
    std::filesystem::remove(path);
    return 0;
}
