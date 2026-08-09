#include <algorithm>
#include <cassert>

#include <taskflow/taskflow.hpp>

#include "agent/internal/agent_thread_state.hpp"
#include "agent/remediation/remediation_graph_template.hpp"
#include "phase4_remediation_test_support.hpp"

int main() {
    using namespace phase4_remediation_test;
    auto p = execution_plan("task-f5r-workflow"); auto c = contract(p); auto r = report(p, c);
    auto a = assurance_checkpoint(p, c, r); auto i = inventory(p, r);
    memory_v2::MemoryProviderRegistry providers; memory_v2::MemoryViewEngine views(providers);
    InMemoryRemediationStore store; planning::InMemoryPlanStore plans; assert(plans.create(p));
    ScriptedModel model; script_success(model);
    LLMRemediationWorkflow workflow(views, store, plans, model);
    const auto result = workflow.run(p, c, r, a, i, subject(p.metadata), options());
    assert(result.state == RemediationState::ReadyForExecution);
    assert(result.impact_graph && result.remediation_plan && result.proposed_plan && result.reverification_plan);
    assert(decode_impact_graph(encode(*result.impact_graph)));
    assert(decode_remediation_plan(encode(*result.remediation_plan)));
    assert(decode_reverification_plan(encode(*result.reverification_plan)));
    assert(result.impact_graph->invalidated_artifact_ids == std::vector<std::string>({"binary", "package"}));
    assert(result.impact_graph->invalidated_evidence_ids ==
           std::vector<std::string>({"evidence-artifact", "evidence-test"}));
    assert(result.proposed_plan->plan_revision == 2);
    assert(result.proposed_plan->parent_plan_digest == planning::encode(p).at("canonical_digest").get<std::string>());
    assert(result.reverification_plan->forced_oracle_kinds ==
           std::vector<std::string>({"artifact", "test"}));
    assert(result.reverification_plan->reusable_evidence_ids == std::vector<std::string>({"evidence-docs"}));
    assert(result.reverification_plan->baseline_artifact_digests.count("docs") == 1);
    assert(std::find(result.reverification_plan->reusable_evidence_ids.begin(),
                     result.reverification_plan->reusable_evidence_ids.end(), "evidence-test") ==
           result.reverification_plan->reusable_evidence_ids.end());
    const auto live = plans.current(p.metadata.identity); assert(live && live->plan_revision == 2);
    assert(model.requests.size() == 3);
    const auto repeated = workflow.run(p, c, r, a, i, subject(p.metadata), options());
    assert(repeated.state == RemediationState::ReadyForExecution && model.requests.size() == 3);

    auto gp = execution_plan("task-f5r-graph"); auto gc = contract(gp); auto gr = report(gp, gc);
    auto ga = assurance_checkpoint(gp, gc, gr); auto gi = inventory(gp, gr);
    memory_v2::MemoryProviderRegistry graph_providers; memory_v2::MemoryViewEngine graph_views(graph_providers);
    InMemoryRemediationStore graph_store; planning::InMemoryPlanStore graph_plans; assert(graph_plans.create(gp));
    ScriptedModel graph_model; script_success(graph_model);
    auto graph_workflow = std::make_shared<LLMRemediationWorkflow>(
        graph_views, graph_store, graph_plans, graph_model);
    auto graph_template = std::make_shared<RemediationGraphTemplate>(
        graph_workflow, gp, gc, gr, ga, gi, subject(gp.metadata), options("graph-f5r"));
    GraphExecutor graph; graph.register_template(graph_template->get_template_name(), graph_template);
    tf::Executor executor; ExecutionRequest request; request.template_id = graph_template->get_template_name();
    request.session = std::make_shared<internal::AgentThreadState>();
    request.session->initial_user_prompt = "Repair the rejected acceptance report.";
    request.context.task_id = gp.metadata.identity.task_id;
    request.context.tenant_id = gp.metadata.identity.tenant_id;
    request.context.session_id = "session-f5r-graph";
    request.options.input_already_processed = true; request.options.persist_session = false;
    const auto executed = graph.execute_sync(executor, request);
    assert(executed.success && executed.outputs.at("state") == "ready_for_execution");
    assert(executed.outputs.contains("impact_graph") && executed.outputs.contains("reverification_plan"));
    return 0;
}
