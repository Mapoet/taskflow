#include <cassert>
#include "agent/conversation/task_execution_snapshot.hpp"

using namespace agent_framework::conversation;
int main() {
    TaskExecutionSnapshot snapshot;
    snapshot.tenant_id="t";snapshot.session_id="s";snapshot.conversation_id="c";
    snapshot.task_id="task";snapshot.run_id="run";snapshot.turn_id="turn";
    snapshot.task_revision=3;snapshot.requirement_revision=2;snapshot.run_revision=7;
    snapshot.plan_revision=1;snapshot.turn_revision=4;snapshot.projection_revision=9;
    assert(validate(snapshot).empty());
    const auto document=encode(snapshot);assert(!document.at("snapshot_digest").get<std::string>().empty());
    SnapshotExpectation expected;expected.task_revision=2;expected.run_revision=7;
    expected.idempotency_key="retry-key";
    auto conflict=compare(snapshot,expected,true);
    assert(!conflict.matches&&conflict.safe_retry&&conflict.changed_fields.size()==1);
    expected.idempotency_key.clear();conflict=compare(snapshot,expected,true);
    assert(!conflict.safe_retry);
    expected.task_revision=3;conflict=compare(snapshot,expected,false);
    assert(conflict.matches);
}
