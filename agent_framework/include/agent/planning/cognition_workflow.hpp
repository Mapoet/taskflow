#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agent/memory_v2/view_engine.hpp"
#include "agent/memory_v2/view_profiles.hpp"
#include "agent/planning/evidence_store.hpp"
#include "agent/planning/plan_store.hpp"
#include "agent/planning/plan_validator.hpp"

namespace agent_framework::planning {

struct InvestigationRequest {
    TaskIntake intake;
    memory_v2::MemoryView view;
    std::string deadline;
    std::uint64_t remaining_tool_calls{0};
    std::string question;
    std::uint64_t round{0};
    std::vector<std::string> prior_evidence_ids;
};

class Investigator {
public:
    virtual ~Investigator() = default;
    virtual std::string id() const = 0;
    virtual bool external() const noexcept = 0;
    virtual bool read_only() const noexcept { return true; }
    virtual std::vector<std::string> required_capabilities() const { return {}; }
    virtual std::vector<EvidenceRecord> investigate(const InvestigationRequest& request,
                                                     std::string* error) = 0;
};

class InvestigatorRegistry {
public:
    bool register_investigator(std::shared_ptr<Investigator> investigator);
    std::vector<std::shared_ptr<Investigator>> all() const;
private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Investigator>> investigators_;
};

struct CognitionDraft {
    TaskUnderstanding understanding;
    ExecutionPlan plan;
};

class CognitionModel {
public:
    virtual ~CognitionModel() = default;
    virtual std::optional<CognitionDraft> draft(const TaskIntake& intake,
                                                 const EvidenceBundle& evidence,
                                                 const memory_v2::MemoryView& planning_view,
                                                 std::string* error) = 0;
};

enum class CognitionOutcome { Approved, AwaitingClarification, AwaitingApproval, Failed };
struct CognitionResult {
    CognitionOutcome outcome{CognitionOutcome::Failed};
    EvidenceBundle evidence;
    std::optional<TaskUnderstanding> understanding;
    std::optional<ExecutionPlan> plan;
    std::vector<PlanIssue> issues;
    std::string error;
};

struct CognitionOptions {
    std::string deadline;
    std::uint64_t investigator_tool_budget{64};
    std::function<bool()> cancelled;
};

class CognitionWorkflow {
public:
    CognitionWorkflow(memory_v2::MemoryViewEngine& views, InvestigatorRegistry& investigators,
                      EvidenceStore& evidence, PlanStore& plans, CognitionModel& model);
    CognitionResult run(const TaskIntake& intake, const memory_v2::MemoryScope& subject,
                        const CognitionOptions& options = {});

private:
    memory_v2::MemoryViewEngine& views_;
    InvestigatorRegistry& investigators_;
    EvidenceStore& evidence_;
    PlanStore& plans_;
    CognitionModel& model_;
    PlanValidator validator_;
};

}  // namespace agent_framework::planning
