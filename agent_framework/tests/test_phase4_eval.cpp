#include <cassert>

#include "agent/eval/runner.hpp"

int main() {
    using namespace agent_framework;
    eval::DatasetRegistry registry;
    for(int i = 0; i < 3; ++i) {
        eval::DatasetCase item;
        item.metadata.identity.tenant_id = "eval-tenant";
        item.metadata.identity.task_id = "case-task-" + std::to_string(i);
        item.case_id = "case-" + std::to_string(i);
        item.input = "input-" + std::to_string(i);
        item.environment = {{"isolation", "fresh"}};
        item.ground_truth = {{"private_label", 1.0}};
        item.criterion_ids = {"success"};
        item.tags = {"offline"};
        item.license = "internal-eval";
        item.dataset_version = "v1";
        assert(registry.register_case(item));
        assert(!registry.register_case(item));
    }
    eval::EvalRunner runner;
    auto execute = [](double score) {
        return [score](const eval::EvalInput& input) {
            assert(!input.environment.contains("private_label"));
            eval::CaseResult result;
            result.trajectory.metadata.identity.tenant_id = "eval-tenant";
            result.trajectory.metadata.identity.task_id = input.case_id;
            result.trajectory.case_id = input.case_id;
            result.trajectory.component_version = "candidate";
            result.trajectory.environment_digest = "sha256:environment";
            result.trajectory.acceptance_report_digest = "sha256:report";
            result.metrics["task_success"] = score;
            result.metrics["scope_leak"] = 0.0;
            return result;
        };
    };
    const auto baseline = runner.run(registry, 42, execute(1.0));
    const auto repeated = runner.run(registry, 42, execute(1.0));
    assert(baseline.run_digest == repeated.run_digest);
    const auto candidate = runner.run(registry, 42, execute(0.5));
    auto metrics = eval::compare(baseline, candidate,
        {{"task_success", 0.05, true}, {"scope_leak", 0.0, false}});
    assert(metrics.size() == 2);
    assert(metrics[0].regression);
    assert(!metrics[1].regression);
    return 0;
}
