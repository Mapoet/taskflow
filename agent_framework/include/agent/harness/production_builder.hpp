#pragma once

#include <memory>
#include <optional>

#include "agent/harness/production_composition.hpp"
#include "agent/harness/production_dependencies.hpp"
#include "agent/harness/production_workflow_adapters.hpp"

namespace agent_framework::harness {

// Boundary adapters are deliberately supplied by the deployment for intake,
// approval and operations. Execution is retained for source compatibility but
// is ignored: production execution is always built from the typed long-task
// dispatcher registry. The builder owns the invariant that no callback/test
// port can enter production.
struct ProductionBoundaryAdapters {
    std::shared_ptr<TypedWorkflowAdapter> intake;
    std::shared_ptr<TypedWorkflowAdapter> approval;
    std::shared_ptr<TypedWorkflowAdapter> execution;
    std::shared_ptr<TypedWorkflowAdapter> operations;
};

struct ProductionBuildReport {
    bool ready{false};
    std::string dependency_manifest_digest;
    std::string composition_manifest_digest;
    std::string deployment_manifest_digest;
    std::vector<ProductionDependencyIssue> dependency_issues;
    std::vector<ProductionCompositionIssue> composition_issues;
    tool_runtime::OrphanRecoveryReport startup_recovery;
    std::size_t startup_timers_processed{0};
};

class DefaultProductionCompositionBuilder {
public:
    explicit DefaultProductionCompositionBuilder(ProductionCompositionDependencies dependencies)
        : dependencies_(std::move(dependencies)) {}

    std::optional<Phase4HarnessRuntime> build(
        const ProductionBoundaryAdapters& boundary,
        ProductionBuildReport* report = nullptr,
        std::string* error = nullptr) const;

private:
    ProductionCompositionDependencies dependencies_;
};

}  // namespace agent_framework::harness
