#include "agent/planning/cognition_workflow.hpp"

#include <algorithm>

namespace agent_framework::planning {

bool InvestigatorRegistry::register_investigator(std::shared_ptr<Investigator> investigator) {
    if(!investigator || investigator->id().empty()) return false;
    std::lock_guard lock(mutex_);
    return investigators_.emplace(investigator->id(), std::move(investigator)).second;
}

std::vector<std::shared_ptr<Investigator>> InvestigatorRegistry::all() const {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<Investigator>> result;
    for(const auto& [id, investigator] : investigators_) { (void)id; result.push_back(investigator); }
    return result;
}

CognitionWorkflow::CognitionWorkflow(
    memory_v2::MemoryViewEngine& views, InvestigatorRegistry& investigators,
    EvidenceStore& evidence, PlanStore& plans, CognitionModel& model)
    : views_(views), investigators_(investigators), evidence_(evidence),
      plans_(plans), model_(model) {}

CognitionResult CognitionWorkflow::run(
    const TaskIntake& intake, const memory_v2::MemoryScope& subject,
    const CognitionOptions& options) {
    CognitionResult result;
    if(intake.user_goal.empty() || intake.metadata.identity.tenant_id.empty() ||
       intake.metadata.identity.task_id.empty()) {
        result.error = "task intake goal and scope are required";
        return result;
    }
    const auto cancelled = [&] { return options.cancelled && options.cancelled(); };
    if(cancelled()) { result.error = "cognition cancelled"; return result; }
    auto investigation_spec = memory_v2::make_view_spec(
        memory_v2::MemoryViewMode::Investigation, intake.metadata, subject);
    auto investigation_view = views_.build(investigation_spec);
    if(investigation_view.fail_closed) {
        result.error = "investigation memory view failed: " + investigation_view.error;
        return result;
    }
    std::vector<std::string> evidence_ids;
    std::uint64_t remaining = options.investigator_tool_budget;
    for(const auto& investigator : investigators_.all()) {
        if(cancelled()) { result.error = "cognition cancelled"; return result; }
        if(remaining == 0) { result.error = "investigation budget exhausted"; return result; }
        std::string error;
        InvestigationRequest request;
        request.intake = intake;
        request.view = investigation_view;
        request.deadline = options.deadline;
        request.remaining_tool_calls = remaining;
        auto records = investigator->investigate(request, &error);
        if(!error.empty()) { result.error = investigator->id() + ":" + error; return result; }
        --remaining;
        for(auto& record : records) {
            if(investigator->external()) record.instruction_authority = false;
            const auto commit = evidence_.append(intake.metadata, record);
            if(commit.status == PlanningCommitStatus::Invalid) {
                result.error = "invalid evidence from " + investigator->id() + ":" + commit.error;
                return result;
            }
            evidence_ids.push_back(record.evidence_id);
        }
    }
    std::sort(evidence_ids.begin(), evidence_ids.end());
    evidence_ids.erase(std::unique(evidence_ids.begin(), evidence_ids.end()), evidence_ids.end());
    result.evidence = evidence_.bundle(intake.metadata, evidence_ids);
    auto planning_spec = memory_v2::make_view_spec(
        memory_v2::MemoryViewMode::Planning, intake.metadata, subject);
    auto planning_view = views_.build(planning_spec);
    if(planning_view.fail_closed) {
        result.error = "planning memory view failed: " + planning_view.error;
        return result;
    }
    std::string draft_error;
    auto draft = model_.draft(intake, result.evidence, planning_view, &draft_error);
    if(!draft) { result.error = "planning failed: " + draft_error; return result; }
    draft->plan.metadata = intake.metadata;
    draft->understanding.metadata = intake.metadata;
    draft->plan.evidence_bundle_digest = encode(result.evidence).at("canonical_digest").get<std::string>();
    draft->plan.task_understanding_digest = encode(draft->understanding).at("canonical_digest").get<std::string>();
    draft->plan.memory_snapshot_id = planning_view.snapshot.snapshot_id;
    draft->plan.planning_view_digest = planning_view.manifest.view_digest;
    result.understanding = draft->understanding;
    result.plan = draft->plan;
    const auto validation = validator_.validate(*result.plan);
    result.issues = validation.issues;
    if(!validation.valid()) { result.error = "plan validation failed"; return result; }
    const auto commit = plans_.create(*result.plan);
    if(!commit) { result.error = "plan persistence failed: " + commit.error; return result; }
    if(!result.understanding->unknowns.empty() || !intake.fact_gaps.empty())
        result.outcome = CognitionOutcome::AwaitingClarification;
    else if(std::any_of(result.plan->nodes.begin(), result.plan->nodes.end(),
                        [](const auto& node) { return node.approval_required; }))
        result.outcome = CognitionOutcome::AwaitingApproval;
    else
        result.outcome = CognitionOutcome::Approved;
    return result;
}

}  // namespace agent_framework::planning
