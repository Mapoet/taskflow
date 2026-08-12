#include <cassert>
#include <memory>

#include "agent/harness/production_builder.hpp"

namespace {
class Boundary final : public agent_framework::harness::TypedWorkflowAdapter {
public:
    Boundary(agent_framework::harness::WorkflowAdapterKind kind,
             agent_framework::harness::HarnessStage stage)
        : kind_(kind), stage_(stage) {}
    std::string id() const override { return "production.boundary"; }
    agent_framework::harness::WorkflowAdapterKind kind() const noexcept override { return kind_; }
    agent_framework::harness::HarnessStage stage() const noexcept override { return stage_; }
    bool side_effecting() const noexcept override { return false; }
    std::string implementation_revision() const override { return "r1"; }
    std::string configuration_digest() const override { return "sha256:config"; }
    agent_framework::harness::WorkflowStageExecution run(
        const agent_framework::harness::HarnessStageRequest&) override { return {}; }
private:
    agent_framework::harness::WorkflowAdapterKind kind_;
    agent_framework::harness::HarnessStage stage_;
};
}

int main() {
    using namespace agent_framework::harness;
    ProductionCompositionDependencies dependencies;
    DefaultProductionCompositionBuilder builder(dependencies);
    ProductionBoundaryAdapters boundary;
    ProductionBuildReport report;
    std::string error;
    assert(!builder.build(boundary, &report, &error));
    assert(!report.ready && !report.dependency_issues.empty());
    assert(report.composition_issues.size() == 4);
    assert(error.find("production_dependency_missing") == 0);

    boundary.intake = std::make_shared<Boundary>(WorkflowAdapterKind::Cognition,
                                                 HarnessStage::Cognition);
    report = {};
    assert(!builder.build(boundary, &report, &error));
    assert(report.composition_issues.size() == 4); // wrong kind cannot masquerade as intake
}
