#include "agent/tool_runtime/recovery_certification.hpp"
#include "agent/tool_runtime/execution_control.hpp"
#include "agent/tool_runtime/store.hpp"
#include "agent/distributed/object_store.hpp"
#include "agent/internal/sqlite_utils.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <unistd.h>
using namespace agent_framework::tool_runtime;
namespace fs = std::filesystem;
int main()
{
    auto root = fs::temp_directory_path() / ("ltw9-recovery-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    auto report = certify_recovery("sha256:env", RecoveryEvidenceLevel::Offline, [](RecoveryScenario s)
                                   {RecoveryCell c;c.scenario=s;c.executed=true;c.passed=true;c.operations=1;c.evidence_digest="sha256:evidence";return c; });
    assert(!report.certified);
    assert(report.blockers.size() == 6); // two process-live + four provider/MCP-live cells cannot skip-as-pass
    auto certified = certify_recovery("sha256:env", RecoveryEvidenceLevel::ProviderLive, [](RecoveryScenario s)
                                      { return RecoveryCell{s, RecoveryEvidenceLevel::Offline, true, true, true, {}, "sha256:e", 1, 0}; });
    assert(certified.certified && certified.cells.size() == mandatory_recovery_scenarios().size());
    assert(encode(certified)["certified"]);
    auto soak = run_recovery_soak({2000, 42, 4}, [](std::uint64_t, std::uint64_t)
                                  { return true; });
    assert(soak.passed && soak.recoveries == 2000 && !soak.evidence_digest.empty());
    auto metrics=certify_long_task_metrics({0,0,0,0,1000,1000,9000,10000});
    assert(metrics.passed&&metrics.blockers.empty()&&!metrics.evidence_digest.empty());
    auto unsafe_metrics=certify_long_task_metrics({1,1,1,1,100,98,10001,15001});
    assert(!unsafe_metrics.passed&&unsafe_metrics.blockers.size()==7&&
           !unsafe_metrics.evidence_digest.empty());
    auto live_path=(root/"provider-live.json").string();auto live=run_provider_live_certification({"sha256:env",live_path,10,7,1},[](RecoveryScenario s){return RecoveryCell{s,RecoveryEvidenceLevel::ProviderLive,true,true,true,{},"sha256:live",1,0};},[](std::uint64_t,std::uint64_t){return true;});assert(live.certified&&fs::exists(live_path));
    auto not_certified=run_provider_live_certification({"sha256:env",{},0,7,1},{},{},nullptr);assert(!not_certified.certified&&!not_certified.blockers.empty());
    auto db = (root / "control.sqlite").string();
    {
        SQLiteExecutionControlStore s(db);
        assert(s.create({"inv", "tenant", 1000, 0, {64, 4, BackpressureMode::Block}}));
        assert(s.request_cancel("inv", "race", 1));
        auto a = s.claim("inv", "worker-a", 1, 5);
        assert(a);
        assert(s.advance("inv", "worker-a", a->fencing_token, a->revision, CancellationStage::Cooperative, 2, false, ""));
    }
    {
        SQLiteExecutionControlStore s(db);
        auto b = s.claim("inv", "worker-b", 7, 5);
        assert(b);
        assert(b->fencing_token >= 2);
        assert(!s.advance("inv", "worker-a", b->fencing_token - 1, b->revision, CancellationStage::Cancelled, 8, true, "sha256:late"));
        assert(s.advance("inv", "worker-b", b->fencing_token, b->revision, CancellationStage::Reconciling, 8, false, ""));
    }
    // Unsupported component schema fails closed instead of silently accepting a future database.
    {
        sqlite3 *d = nullptr;
        assert(sqlite3_open(db.c_str(), &d) == SQLITE_OK);
        assert(sqlite3_exec(d, "UPDATE agent_component_schema SET version=99 WHERE component='execution_control'", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(d);
        bool rejected = false;
        try
        {
            SQLiteExecutionControlStore incompatible(db);
        }
        catch (...)
        {
            rejected = true;
        }
        assert(rejected);
    }
    // Busy/locked is classified and never reported committed.
    auto busydb = (root / "busy.sqlite").string();
    SQLiteInvocationStore first(busydb, 1);
    LongRunningToolInvocation seed;
    seed.invocation_id = "seed";
    seed.metadata.identity.tenant_id = "tenant";
    seed.metadata.identity.run_id = "run";
    seed.metadata.identity.task_id = "task";
    seed.conversation_id = "conversation";
    seed.turn_id = "turn";
    seed.tool_call_id = "seed-call";
    seed.tool_name = "tool";
    seed.tool_contract_revision = "v1";
    seed.deployment_revision = "d1";
    seed.tool_generation = "g1";
    seed.input_digest = "sha256:i";
    seed.created_at = seed.updated_at = "now";
    assert(first.create(seed));
    sqlite3 *lock = nullptr;
    assert(sqlite3_open(busydb.c_str(), &lock) == SQLITE_OK);
    assert(sqlite3_exec(lock, "BEGIN IMMEDIATE;UPDATE tool_invocations SET tool_name=tool_name WHERE invocation_id='seed'", nullptr, nullptr, nullptr) == SQLITE_OK);
    auto v = seed;
    v.invocation_id = "busy";
    v.tool_call_id = "busy-call";
    auto busy = first.create(v);
    assert(!busy && (busy.status == InvocationStoreStatus::Busy || busy.status == InvocationStoreStatus::Error));
    sqlite3_exec(lock, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(lock);
    // Content-addressed reads detect on-disk corruption.
    agent_framework::distributed::FilesystemObjectStore objects(root / "objects");
    std::string error;
    auto object = objects.put("tenant", "payload", "text/plain", {}, &error);
    assert(object);
    auto object_path = root / "objects" / "tenant" / object->digest.substr(7, 2) / object->digest.substr(7);
    {
        std::ofstream out(object_path, std::ios::binary | std::ios::trunc);
        out << "tampered";
    }
    assert(!objects.get(*object, &error));
    fs::remove_all(root);
    return 0;
}
