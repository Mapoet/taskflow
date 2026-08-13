#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>
#include "agent/tool_runtime/types.hpp"
#include "agent/tool_runtime/execution_control.hpp"

namespace agent_framework::tool_runtime
{
enum class ExecutionAdapterKind { LocalTool, Bubblewrap, MCP, HTTP, A2AChildTask };
enum class AdapterOrigin { Production, Test, Scripted };
enum class RestartPolicy { Attach, RestartFromCheckpoint, RestartFromInput, ManualReview };
enum class ObservationState { Accepted, Running, Progress, CompletedCandidate, Failed, Cancelled, Unknown };

struct ExecutionAdapterCapabilities
{
    bool attach{false}, query{false}, cancel{false}, checkpoint{false};
    bool result{true}, reconcile_effect{false}, idempotency_key{false};
};
struct ExecutionRequest
{
    LongRunningToolInvocation invocation;
    nlohmann::json input=nlohmann::json::object();
    std::string idempotency_key, checkpoint_ref;
    std::uint64_t fencing_token{0};
    ExecutionControlEnvelope control;
};
struct ExecutionHandle
{
    std::string adapter_id, adapter_revision, deployment_generation, external_id;
    std::uint64_t fencing_token{0};
};
struct ExecutionObservation
{
    ObservationState state{ObservationState::Unknown};
    nlohmann::json result=nlohmann::json::object();
    std::string result_digest, checkpoint_ref, error_code;
    bool effect_known{false}, information_gain{false};
    std::optional<PartialResultRef> incremental_result;
};
struct CancellationResult { bool accepted{false}, terminal{false}; std::string diagnostic; bool effect_known{false}; std::string receipt_digest; };
struct ReconciliationResult { ExecutionObservation observation; bool safe_to_retry{false}; };

class ExecutionAdapter
{
public:
    virtual ~ExecutionAdapter()=default;
    virtual std::string id()const=0;
    virtual std::string revision()const=0;
    virtual std::string deployment_generation()const=0;
    virtual ExecutionAdapterKind kind()const noexcept=0;
    virtual AdapterOrigin origin()const noexcept=0;
    virtual ExecutionAdapterCapabilities capabilities()const noexcept=0;
    virtual RestartPolicy restart_policy()const noexcept=0;
    virtual std::optional<ExecutionHandle> start(const ExecutionRequest&,std::string*)=0;
    virtual std::optional<ExecutionHandle> attach(const ExecutionRequest&,std::string*);
    virtual ExecutionObservation query(const ExecutionHandle&)=0;
    virtual CancellationResult cancel(const ExecutionHandle&)=0;
    virtual CancellationResult escalate(const ExecutionHandle&, CancellationStage);
    virtual ReconciliationResult reconcile(const ExecutionRequest&,const ExecutionHandle&)=0;
};

class ExecutionAdapterRegistry
{
public:
    explicit ExecutionAdapterRegistry(bool production_profile=true):production_(production_profile){}
    bool register_adapter(std::shared_ptr<ExecutionAdapter>,std::string* error=nullptr);
    std::shared_ptr<ExecutionAdapter> find(std::string_view id,std::string_view revision,
                                           std::string_view generation)const;
private:
    bool production_;mutable std::mutex mutex_;
    std::map<std::string,std::shared_ptr<ExecutionAdapter>> adapters_;
};

using AdapterStartFn=std::function<std::optional<ExecutionHandle>(const ExecutionRequest&,std::string*)>;
using AdapterQueryFn=std::function<ExecutionObservation(const ExecutionHandle&)>;
using AdapterCancelFn=std::function<CancellationResult(const ExecutionHandle&)>;
using AdapterReconcileFn=std::function<ReconciliationResult(const ExecutionRequest&,const ExecutionHandle&)>;

struct CallbackAdapterSpec
{
    std::string id,revision,generation;
    ExecutionAdapterKind kind{ExecutionAdapterKind::LocalTool};
    AdapterOrigin origin{AdapterOrigin::Production};
    ExecutionAdapterCapabilities capabilities;
    RestartPolicy restart_policy{RestartPolicy::ManualReview};
};

class CallbackExecutionAdapter final:public ExecutionAdapter
{
public:
    CallbackExecutionAdapter(CallbackAdapterSpec,AdapterStartFn,AdapterQueryFn,
                             AdapterCancelFn={},AdapterReconcileFn={});
    std::string id()const override{return spec_.id;}std::string revision()const override{return spec_.revision;}
    std::string deployment_generation()const override{return spec_.generation;}
    ExecutionAdapterKind kind()const noexcept override{return spec_.kind;}AdapterOrigin origin()const noexcept override{return spec_.origin;}
    ExecutionAdapterCapabilities capabilities()const noexcept override{return spec_.capabilities;}
    RestartPolicy restart_policy()const noexcept override{return spec_.restart_policy;}
    std::optional<ExecutionHandle> start(const ExecutionRequest&,std::string*)override;
    std::optional<ExecutionHandle> attach(const ExecutionRequest&,std::string*)override;
    ExecutionObservation query(const ExecutionHandle&)override;CancellationResult cancel(const ExecutionHandle&)override;
    ReconciliationResult reconcile(const ExecutionRequest&,const ExecutionHandle&)override;
private:CallbackAdapterSpec spec_;AdapterStartFn start_;AdapterQueryFn query_;AdapterCancelFn cancel_;AdapterReconcileFn reconcile_;
};
}
