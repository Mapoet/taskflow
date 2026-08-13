#pragma once
#include <atomic>
#include <future>
#include <map>
#include "agent/tool_runtime/execution_adapter.hpp"
#include "agent/toolbus/toolbus.hpp"
#include "agent/sandbox/provider.hpp"
#include "agent/agent/child_task.hpp"

namespace agent_framework::tool_runtime
{
class AsyncExecutionAdapter:public ExecutionAdapter
{
public:
    ExecutionObservation query(const ExecutionHandle&)override;
    CancellationResult cancel(const ExecutionHandle&)override;
    CancellationResult escalate(const ExecutionHandle&, CancellationStage) override;
    ReconciliationResult reconcile(const ExecutionRequest&,const ExecutionHandle&)override;
protected:
    struct Operation{std::shared_ptr<std::atomic_bool> cancel=std::make_shared<std::atomic_bool>(false);std::future<ExecutionObservation> future;std::optional<ExecutionObservation> terminal;};
    std::optional<ExecutionHandle> launch(const ExecutionRequest&,std::function<ExecutionObservation(std::shared_ptr<std::atomic_bool>)>,std::string*);
    mutable std::mutex operations_mutex_;std::map<std::string,std::shared_ptr<Operation>> operations_;
};

class ToolBusExecutionAdapter final:public AsyncExecutionAdapter
{
public:
    ToolBusExecutionAdapter(std::shared_ptr<ToolBus>,std::string id,std::string revision,
                            std::string generation,ExecutionAdapterKind kind=ExecutionAdapterKind::LocalTool);
    std::string id()const override{return id_;}std::string revision()const override{return revision_;}std::string deployment_generation()const override{return generation_;}
    ExecutionAdapterKind kind()const noexcept override{return kind_;}AdapterOrigin origin()const noexcept override{return AdapterOrigin::Production;}
    ExecutionAdapterCapabilities capabilities()const noexcept override{return {false,true,true,false,true,false,true};}
    RestartPolicy restart_policy()const noexcept override{return RestartPolicy::RestartFromInput;}
    std::optional<ExecutionHandle> start(const ExecutionRequest&,std::string*)override;
private:std::shared_ptr<ToolBus> bus_;std::string id_,revision_,generation_;ExecutionAdapterKind kind_;
};

/** Durable typed network adapter. Wget resumes from its .part/ETag checkpoint;
 * Curl is fail-closed on process loss because a remote mutation may be uncertain. */
class NetworkToolExecutionAdapter final:public AsyncExecutionAdapter
{
public:
    NetworkToolExecutionAdapter(std::shared_ptr<ToolBus>,std::string tool_name,
                                std::string revision,std::string generation);
    std::string id()const override{return id_;}std::string revision()const override{return revision_;}
    std::string deployment_generation()const override{return generation_;}
    ExecutionAdapterKind kind()const noexcept override{return ExecutionAdapterKind::HTTP;}
    AdapterOrigin origin()const noexcept override{return AdapterOrigin::Production;}
    ExecutionAdapterCapabilities capabilities()const noexcept override{return {false,true,true,tool_name_=="Wget",true,true,true};}
    RestartPolicy restart_policy()const noexcept override{return tool_name_=="Wget"?RestartPolicy::RestartFromCheckpoint:RestartPolicy::ManualReview;}
    std::optional<ExecutionHandle> start(const ExecutionRequest&,std::string*)override;
    ReconciliationResult reconcile(const ExecutionRequest&,const ExecutionHandle&)override;
private:
    std::shared_ptr<ToolBus> bus_;std::string tool_name_,id_,revision_,generation_;
};

class BubblewrapExecutionAdapter final:public AsyncExecutionAdapter
{
public:
    BubblewrapExecutionAdapter(std::shared_ptr<sandbox::SandboxProvider>,sandbox::SandboxSpec,
                               std::string revision,std::string generation);
    std::string id()const override{return "bubblewrap";}std::string revision()const override{return revision_;}std::string deployment_generation()const override{return generation_;}
    ExecutionAdapterKind kind()const noexcept override{return ExecutionAdapterKind::Bubblewrap;}AdapterOrigin origin()const noexcept override{return AdapterOrigin::Production;}
    ExecutionAdapterCapabilities capabilities()const noexcept override{return {false,true,false,true,true,false,true};}
    RestartPolicy restart_policy()const noexcept override{return RestartPolicy::RestartFromCheckpoint;}
    std::optional<ExecutionHandle> start(const ExecutionRequest&,std::string*)override;
    CancellationResult escalate(const ExecutionHandle&,CancellationStage)override;
private:std::shared_ptr<sandbox::SandboxProvider> provider_;sandbox::SandboxSpec spec_;std::string revision_,generation_;
    std::mutex handles_mutex_;std::map<std::string,sandbox::SandboxHandle> handles_;
};

class ChildTaskExecutionAdapter final:public AsyncExecutionAdapter
{
public:
    ChildTaskExecutionAdapter(std::shared_ptr<ChildTaskBackend>,ChildTaskRequest,
                              std::string id,std::string revision,std::string generation,bool remote);
    std::string id()const override{return id_;}std::string revision()const override{return revision_;}std::string deployment_generation()const override{return generation_;}
    ExecutionAdapterKind kind()const noexcept override{return ExecutionAdapterKind::A2AChildTask;}AdapterOrigin origin()const noexcept override{return AdapterOrigin::Production;}
    ExecutionAdapterCapabilities capabilities()const noexcept override{return {false,true,true,true,true,true,true};}
    RestartPolicy restart_policy()const noexcept override{return remote_?RestartPolicy::ManualReview:RestartPolicy::RestartFromCheckpoint;}
    std::optional<ExecutionHandle> start(const ExecutionRequest&,std::string*)override;
private:std::shared_ptr<ChildTaskBackend> backend_;ChildTaskRequest request_;std::string id_,revision_,generation_;bool remote_;
};

class HTTPExecutionAdapter final:public ExecutionAdapter
{
public:
    using Start=std::function<std::optional<std::string>(const ExecutionRequest&,std::string*)>;
    using Query=std::function<ExecutionObservation(std::string_view)>;
    using Cancel=std::function<CancellationResult(std::string_view)>;
    using Reconcile=std::function<ReconciliationResult(const ExecutionRequest&,std::string_view)>;
    HTTPExecutionAdapter(std::string,std::string,std::string,Start,Query,Cancel={},Reconcile={});
    std::string id()const override{return id_;}std::string revision()const override{return revision_;}std::string deployment_generation()const override{return generation_;}
    ExecutionAdapterKind kind()const noexcept override{return ExecutionAdapterKind::HTTP;}AdapterOrigin origin()const noexcept override{return AdapterOrigin::Production;}
    ExecutionAdapterCapabilities capabilities()const noexcept override{return {true,true,bool(cancel_),true,true,bool(reconcile_),true};}
    RestartPolicy restart_policy()const noexcept override{return RestartPolicy::Attach;}
    std::optional<ExecutionHandle> start(const ExecutionRequest&,std::string*)override;
    std::optional<ExecutionHandle> attach(const ExecutionRequest&,std::string*)override;
    ExecutionObservation query(const ExecutionHandle&)override;CancellationResult cancel(const ExecutionHandle&)override;
    ReconciliationResult reconcile(const ExecutionRequest&,const ExecutionHandle&)override;
private:std::string id_,revision_,generation_;Start start_;Query query_;Cancel cancel_;Reconcile reconcile_;
};

struct RemoteExecutionIdentity { std::string external_id,session_id,task_id,peer_id; };
class RemoteExecutionProtocol {
public: virtual ~RemoteExecutionProtocol()=default;
    virtual std::optional<RemoteExecutionIdentity> start(const ExecutionRequest&,std::string*)=0;
    virtual ExecutionObservation query(const RemoteExecutionIdentity&)=0;
    virtual CancellationResult cancel(const RemoteExecutionIdentity&,CancellationStage)=0;
    virtual ReconciliationResult reconcile(const ExecutionRequest&,const RemoteExecutionIdentity&)=0;
    virtual bool attach(const RemoteExecutionIdentity&,std::string*)=0;
};
class RemoteProtocolExecutionAdapter final:public ExecutionAdapter {
public: RemoteProtocolExecutionAdapter(std::shared_ptr<RemoteExecutionProtocol>,ExecutionAdapterKind,std::string,std::string,std::string,RestartPolicy=RestartPolicy::Attach);
    std::string id()const override{return id_;}std::string revision()const override{return revision_;}std::string deployment_generation()const override{return generation_;}
    ExecutionAdapterKind kind()const noexcept override{return kind_;}AdapterOrigin origin()const noexcept override{return AdapterOrigin::Production;}
    ExecutionAdapterCapabilities capabilities()const noexcept override{return {true,true,true,true,true,true,true};}RestartPolicy restart_policy()const noexcept override{return restart_;}
    std::optional<ExecutionHandle> start(const ExecutionRequest&,std::string*)override;std::optional<ExecutionHandle> attach(const ExecutionRequest&,std::string*)override;
    ExecutionObservation query(const ExecutionHandle&)override;CancellationResult cancel(const ExecutionHandle&)override;CancellationResult escalate(const ExecutionHandle&,CancellationStage)override;ReconciliationResult reconcile(const ExecutionRequest&,const ExecutionHandle&)override;
private:RemoteExecutionIdentity identity(const ExecutionHandle&)const;std::shared_ptr<RemoteExecutionProtocol> protocol_;ExecutionAdapterKind kind_;std::string id_,revision_,generation_;RestartPolicy restart_;
};
}
