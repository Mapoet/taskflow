#pragma once
#include "agent/tool_runtime/store.hpp"
#include "agent/toolbus/tool_effect_journal.hpp"
#include "agent/run/store.hpp"
#include "agent/harness/cross_store_coordination.hpp"
#include <functional>
namespace agent_framework::tool_runtime
{
struct InvocationCommitRequest
{
    std::string invocation_id,tenant_id,run_id,harness_id,idempotency_key;
    std::string request_digest,result_media_type{"application/json"};
    nlohmann::json result=nlohmann::json::object();
    std::uint64_t fencing_token{0};
    bool effect_known{false},idempotent{false};
};
struct InvocationCommitOutcome
{
    bool committed{false},manual_review{false};
    std::string result_digest,artifact_digest,coordination_receipt,error;
};
class InvocationCommitCoordinator
{
public:
    using FaultInjector=std::function<void(std::string_view)>;
    InvocationCommitCoordinator(InvocationStore&,distributed::ObjectStore&,ToolEffectJournal&,
                                run::RunStore&,harness::CrossStoreCoordinator&,FaultInjector={});
    InvocationCommitOutcome reserve(const InvocationCommitRequest&);
    InvocationCommitOutcome commit(const InvocationCommitRequest&);
    InvocationCommitOutcome reconcile(const InvocationCommitRequest&);
    std::size_t sweep_orphans(const std::function<std::optional<InvocationCommitRequest>(const ToolEffectRecord&)>&,
                              std::size_t limit=100);
private:
    InvocationCommitOutcome drive(const InvocationCommitRequest&,bool);
    InvocationStore& invocations_;distributed::ObjectStore& objects_;ToolEffectJournal& effects_;
    run::RunStore& runs_;harness::CrossStoreCoordinator& coordination_;
    FaultInjector fault_;
};
}
