#include <cassert>

#include "agent/live/production_bundle.hpp"
#include "phase4_live_test_support.hpp"

int main() {
    using namespace agent_framework::live;
    using namespace phase4_live_test;
    auto environment_value = environment("task-r6l-bundle");
    auto matrix_value = matrix(environment_value, false);
    nlohmann::json document = {
        {"evidence_level", "production-certified"},
        {"environment", encode(environment_value)},
        {"matrix", encode(matrix_value)},
        {"mandatory_cell_ids", nlohmann::json::array()},
        {"mandatory_dependency_digests", environment_value.dependency_digests}};
    for(const auto& cell : matrix_value.cells)
        document["mandatory_cell_ids"].push_back(cell.cell_id);
    std::vector<std::string> errors;
    const auto bundle = decode_production_live_bundle(document, &errors);
    assert(bundle && errors.empty());
    assert(bundle->evidence_level == LiveEvidenceLevel::ProductionCertified);

    auto unknown = document;
    unknown["allow_skip"] = true;
    assert(!decode_production_live_bundle(unknown, &errors));
    auto missing_cell = document;
    missing_cell["mandatory_cell_ids"].push_back("not-present");
    assert(!decode_production_live_bundle(missing_cell, &errors));
    auto literal_secret = environment_value;
    literal_secret.secret_refs = {"literal-secret"};
    auto invalid_secret = document;
    invalid_secret["environment"] = encode(literal_secret);
    auto invalid_matrix = matrix_value;
    invalid_matrix.environment_digest = role_environment_digest(literal_secret);
    invalid_secret["matrix"] = encode(invalid_matrix);
    assert(!decode_production_live_bundle(invalid_secret, &errors));
    return 0;
}
