#include <cassert>

#include "phase4_remediation_test_support.hpp"

int main() {
    using namespace phase4_remediation_test;
    {
        auto p = execution_plan("task-f5r-anti-downgrade"); auto c = contract(p); auto r = report(p, c);
        auto a = assurance_checkpoint(p, c, r); auto i = inventory(p, r);
        memory_v2::MemoryProviderRegistry providers; memory_v2::MemoryViewEngine views(providers);
        InMemoryRemediationStore store; planning::InMemoryPlanStore plans; assert(plans.create(p));
        ScriptedModel model; script_success(model, true);
        LLMRemediationWorkflow workflow(views, store, plans, model);
        auto o = options("anti-downgrade"); o.approval_decision_id.clear(); o.approval_validator = {};
        const auto awaiting = workflow.run(p, c, r, a, i, subject(p.metadata), o);
        assert(awaiting.state == RemediationState::AwaitingApproval);
        assert(awaiting.error_code == "remediation_approval_required");
        const auto unchanged = plans.current(p.metadata.identity); assert(unchanged && unchanged->plan_revision == 1);
        o.approval_decision_id = "decision-f5r";
        o.approval_validator = [](std::string_view request, std::string_view decision) {
            return request.rfind("sha256:", 0) == 0 && decision == "decision-f5r";
        };
        const auto approved = workflow.run(p, c, r, a, i, subject(p.metadata), o);
        assert(approved.state == RemediationState::ReadyForExecution);
        assert(approved.checkpoint.approval_decision_id == "decision-f5r");
        assert(approved.proposed_plan->acceptance_contract_digest != p.acceptance_contract_digest);
    }
    {
        auto p = execution_plan("task-f5r-capability"); auto c = contract(p); auto r = report(p, c);
        auto a = assurance_checkpoint(p, c, r); auto i = inventory(p, r);
        memory_v2::MemoryProviderRegistry providers; memory_v2::MemoryViewEngine views(providers);
        InMemoryRemediationStore store; planning::InMemoryPlanStore plans; assert(plans.create(p));
        ScriptedModel model; model.push(RemediationStage::ImpactAnalysis, impact_output());
        model.push(RemediationStage::RemediationPlanning, planner_output(false, true));
        model.push(RemediationStage::RemediationPlanning, planner_output(false, true));
        LLMRemediationWorkflow workflow(views, store, plans, model);
        const auto denied = workflow.run(p, c, r, a, i, subject(p.metadata), options("capability"));
        assert(denied.state == RemediationState::ManualReview);
        assert(denied.error_code == "remediation_plan_invalid");
    }
    {
        auto p = execution_plan("task-f5r-budget"); auto c = contract(p); auto r = report(p, c);
        auto a = assurance_checkpoint(p, c, r); auto i = inventory(p, r);
        memory_v2::MemoryProviderRegistry providers; memory_v2::MemoryViewEngine views(providers);
        InMemoryRemediationStore store; planning::InMemoryPlanStore plans; assert(plans.create(p));
        ScriptedModel model; script_success(model);
        LLMRemediationWorkflow workflow(views, store, plans, model);
        auto o = options("budget"); o.token_budget = 100;
        const auto bounded = workflow.run(p, c, r, a, i, subject(p.metadata), o);
        assert(bounded.state == RemediationState::ManualReview);
        assert(bounded.error_code == "remediation_budget_exhausted");
    }
    {
        auto p = execution_plan("task-f5r-duplicate-finding"); auto c = contract(p); auto r = report(p, c);
        r.findings.push_back(r.findings.front());
        auto a = assurance_checkpoint(p, c, r); auto i = inventory(p, r);
        memory_v2::MemoryProviderRegistry providers; memory_v2::MemoryViewEngine views(providers);
        InMemoryRemediationStore store; planning::InMemoryPlanStore plans; assert(plans.create(p));
        ScriptedModel model; LLMRemediationWorkflow workflow(views, store, plans, model);
        const auto duplicate = workflow.run(p, c, r, a, i, subject(p.metadata), options("duplicate"));
        assert(duplicate.state == RemediationState::Failed);
        assert(duplicate.error_code == "remediation_input_invalid");
    }
    {
        auto p = execution_plan("task-f5r-loop"); auto c = contract(p); auto r = report(p, c);
        auto a = assurance_checkpoint(p, c, r); auto i = inventory(p, r);
        memory_v2::MemoryProviderRegistry providers; memory_v2::MemoryViewEngine views(providers);
        InMemoryRemediationStore store; planning::InMemoryPlanStore plans; assert(plans.create(p));
        ScriptedModel model; script_success(model);
        LLMRemediationWorkflow workflow(views, store, plans, model);
        auto o = options("loop"); o.approval_decision_id.clear(); o.approval_validator = {};
        const auto awaiting = workflow.run(p, c, r, a, i, subject(p.metadata), o);
        assert(awaiting.state == RemediationState::AwaitingApproval);
        auto stored = store.load("tenant-a", "loop"); assert(stored);
        auto replan = stored->checkpoint; replan.revision = stored->revision + 1;
        replan.state = RemediationState::Running; replan.next_stage = RemediationStage::RemediationPlanning;
        replan.remediation_plan.reset(); replan.proposed_plan.reset();
        assert(store.compare_exchange(replan, stored->revision));
        model.push(RemediationStage::RemediationPlanning, planner_output());
        const auto looped = workflow.run(p, c, r, a, i, subject(p.metadata), o);
        assert(looped.state == RemediationState::ManualReview);
        assert(looped.error_code == "remediation_loop_detected");
    }
    return 0;
}
