#include "agent/conversation/task_execution_snapshot.hpp"
#include "agent/contracts/contract.hpp"

namespace agent_framework::conversation {
namespace {
using nlohmann::json;
json payload(const TaskExecutionSnapshot& value) {
    return {{"schema","agent.task_execution_snapshot/v1"},
        {"tenant_id",value.tenant_id},{"session_id",value.session_id},
        {"conversation_id",value.conversation_id},{"task_id",value.task_id},
        {"run_id",value.run_id},{"turn_id",value.turn_id},{"plan_id",value.plan_id},
        {"task_revision",value.task_revision},{"requirement_revision",value.requirement_revision},
        {"plan_revision",value.plan_revision},{"run_revision",value.run_revision},
        {"turn_revision",value.turn_revision},{"projection_revision",value.projection_revision},
        {"task_digest",value.task_digest},{"requirement_digest",value.requirement_digest},
        {"plan_digest",value.plan_digest},{"run_digest",value.run_digest},
        {"projection_digest",value.projection_digest}};
}
}
nlohmann::json encode(const TaskExecutionSnapshot& value) {
    auto out=payload(value);out["snapshot_digest"]=value.snapshot_digest.empty()
        ?contracts::canonical_digest(out).value_or(""):value.snapshot_digest;return out;
}
std::vector<std::string> validate(const TaskExecutionSnapshot& value) {
    std::vector<std::string> errors;
    if(value.tenant_id.empty()||value.session_id.empty()||value.conversation_id.empty()||
       value.task_id.empty()||value.run_id.empty())errors.push_back("snapshot_identity_required");
    if(value.task_revision==0||value.requirement_revision==0||value.run_revision==0)
        errors.push_back("snapshot_authoritative_revision_required");
    const auto digest=contracts::canonical_digest(payload(value)).value_or("");
    if(!value.snapshot_digest.empty()&&value.snapshot_digest!=digest)
        errors.push_back("snapshot_digest_mismatch");
    return errors;
}
SnapshotConflict compare(const TaskExecutionSnapshot& current,const SnapshotExpectation& expected,
    bool command_is_idempotent) {
    SnapshotConflict out;out.current=current;
    const auto check=[&](const auto& value,std::uint64_t actual,const char* field) {
        if(value&&*value!=actual)out.changed_fields.push_back(field);};
    check(expected.task_revision,current.task_revision,"task_revision");
    check(expected.requirement_revision,current.requirement_revision,"requirement_revision");
    check(expected.plan_revision,current.plan_revision,"plan_revision");
    check(expected.run_revision,current.run_revision,"run_revision");
    check(expected.turn_revision,current.turn_revision,"turn_revision");
    check(expected.projection_revision,current.projection_revision,"projection_revision");
    out.matches=out.changed_fields.empty();
    out.safe_retry=!out.matches&&command_is_idempotent&&!expected.idempotency_key.empty();
    return out;
}
}  // namespace agent_framework::conversation
