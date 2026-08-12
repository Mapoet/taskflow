#include "agent/conversation/production_bridge.hpp"
namespace agent_framework::conversation
{
  ProfileDecision TaskProfileRouter::route(TaskExecutionProfile requested, bool effect, bool production)
  {
    ProfileDecision d{true, requested, "profile_accepted", requested != TaskExecutionProfile::Conversation};
    if (effect && (requested == TaskExecutionProfile::Conversation || requested == TaskExecutionProfile::ReadOnlyAnalysis))
    {
      if (production)
        return {false, requested, "side_effect_requires_profile_upgrade", true};
      d.profile = TaskExecutionProfile::ArtifactDelivery;
      d.reason_code = "profile_upgraded_for_side_effect";
      d.requires_harness = true;
    }
    if (production && requested == TaskExecutionProfile::Conversation)
      return {true, requested, "conversation_unverified", false};
    return d;
  }
  std::optional<harness::TaskClosureContract> closure_contract_from(const assurance::AcceptanceContract &a, TaskExecutionProfile p, std::string *e)
  {
    harness::TaskClosureContract c;
    c.metadata = a.metadata;
    c.contract_id = "closure:" + a.metadata.identity.task_id;
    c.revision = std::to_string(a.revision);
    c.task_class = std::string(name(p));
    c.clarification_policy = "ask";
    c.max_remediation_cycles = 2;
    for (const auto &x : a.criteria)
    {
      if (!x.mandatory)
        continue;
      c.mandatory_criteria.push_back(x.criterion_id);
      c.verification_methods[x.criterion_id] = {x.oracle_kind};
    }
    if (auto problems = harness::validate(c); !problems.empty())
    {
      if (e)
        *e = problems.front();
      return std::nullopt;
    }
    return c;
  }
  bool outcome_can_close_task(const ModelTurnOutcome &) noexcept { return false; }
}
