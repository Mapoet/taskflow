#include "agent/tool_runtime/typed_adapters.hpp"
#include "agent/contracts/contract.hpp"
#include <chrono>
namespace agent_framework::tool_runtime
{
    namespace
    {
        std::atomic_uint64_t ids{1};
        std::string digest(const nlohmann::json &v) { return contracts::canonical_digest(v).value_or(""); }
    }
    std::optional<ExecutionHandle> AsyncExecutionAdapter::launch(const ExecutionRequest &r, std::function<ExecutionObservation(std::shared_ptr<std::atomic_bool>)> fn, std::string *e)
    {
        try
        {
            auto op = std::make_shared<Operation>();
            const auto external = id() + "-" + std::to_string(ids++);
            op->future = std::async(std::launch::async, [fn = std::move(fn), c = op->cancel]
                                    {try{return fn(c);}catch(const std::exception&x){return ExecutionObservation{ObservationState::Failed,{}, {},{},x.what(),false,true};} });
            {
                std::lock_guard l(operations_mutex_);
                operations_[external] = op;
            }
            return ExecutionHandle{id(), revision(), deployment_generation(), external, r.fencing_token};
        }
        catch (const std::exception &x)
        {
            if (e)
                *e = x.what();
            return {};
        }
    }
    ExecutionObservation AsyncExecutionAdapter::query(const ExecutionHandle &h)
    {
        std::shared_ptr<Operation> op;
        {
            std::lock_guard l(operations_mutex_);
            auto it = operations_.find(h.external_id);
            if (it == operations_.end())
                return {ObservationState::Unknown, {}, {}, {}, "operation_not_found", false, false};
            op = it->second;
        }
        if (op->terminal)
            return *op->terminal;
        if (op->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            return {ObservationState::Running, {}, {}, {}, {}, false, false};
        op->terminal = op->future.get();
        return *op->terminal;
    }
    CancellationResult AsyncExecutionAdapter::cancel(const ExecutionHandle &h)
    {
        std::lock_guard l(operations_mutex_);
        auto it = operations_.find(h.external_id);
        if (it == operations_.end())
            return {false, false, "operation_not_found"};
        it->second->cancel->store(true);
        return {true, false, "cooperative_cancel_requested"};
    }
    CancellationResult AsyncExecutionAdapter::escalate(const ExecutionHandle &h, CancellationStage stage)
    {
        std::lock_guard l(operations_mutex_);
        auto it = operations_.find(h.external_id);
        if (it == operations_.end())
            return {false, false, "operation_not_found", false, {}};
        it->second->cancel->store(true);
        if (it->second->terminal && it->second->terminal->state == ObservationState::Cancelled)
            return {true, true, "cancel_observed", true, it->second->terminal->result_digest};
        return {true, false, stage == CancellationStage::Kill ? "kill_requested; awaiting effect reconciliation" : stage == CancellationStage::Terminate ? "terminate_requested; awaiting effect reconciliation"
                                                                                                                                                         : "cooperative_cancel_requested",
                false,
                {}};
    }
    ReconciliationResult AsyncExecutionAdapter::reconcile(const ExecutionRequest &r, const ExecutionHandle &h)
    {
        auto o = query(h);
        const bool retry = o.state == ObservationState::Unknown && r.invocation.idempotent;
        return {std::move(o), retry};
    }
    ToolBusExecutionAdapter::ToolBusExecutionAdapter(std::shared_ptr<ToolBus> b, std::string i, std::string r, std::string g, ExecutionAdapterKind k) : bus_(std::move(b)), id_(std::move(i)), revision_(std::move(r)), generation_(std::move(g)), kind_(k)
    {
        if (!bus_)
            throw std::invalid_argument("ToolBus required");
    }
    std::optional<ExecutionHandle> ToolBusExecutionAdapter::start(const ExecutionRequest &r, std::string *e)
    {
        return launch(r, [this, r](auto c)
                      {ToolCallControl control;control.cancellation_requested=[c]{return c->load();};auto value=bus_->call_tool(r.invocation.tool_name,r.input,control).get();const bool cancelled=value.value("code","")=="cancelled";const bool failed=value.contains("error")&&!cancelled;auto state=cancelled?ObservationState::Cancelled:failed?ObservationState::Failed:ObservationState::CompletedCandidate;return ExecutionObservation{state,value,digest(value),{},failed?value.value("code","tool_failed"):"",!failed,!failed}; }, e);
    }
    NetworkToolExecutionAdapter::NetworkToolExecutionAdapter(std::shared_ptr<ToolBus> bus,
        std::string tool,std::string revision,std::string generation)
        :bus_(std::move(bus)),tool_name_(std::move(tool)),id_(tool_name_=="Wget"?"network-wget":"network-curl"),
         revision_(std::move(revision)),generation_(std::move(generation))
    {
        if(!bus_||(tool_name_!="Wget"&&tool_name_!="Curl"))throw std::invalid_argument("Curl or Wget ToolBus adapter required");
    }
    std::optional<ExecutionHandle> NetworkToolExecutionAdapter::start(const ExecutionRequest&r,std::string*e)
    {
        if(r.invocation.tool_name!=tool_name_){if(e)*e="invocation tool does not match network adapter";return {};}
        if(tool_name_=="Wget" && !r.input.value("resume",false)) {
            if(e)*e="durable Wget requires resume=true";return {};
        }
        return launch(r,[this,r](auto cancelled){
            ToolCallControl control;control.cancellation_requested=[cancelled]{return cancelled->load();};
            auto value=bus_->call_tool(tool_name_,r.input,control).get();
            if(cancelled->load())return ExecutionObservation{ObservationState::Cancelled,{}, {},r.input.value("output_path","")+".part","cancelled",false,true};
            const bool failed=value.contains("error");
            const bool uncertain=tool_name_=="Curl"&&!r.invocation.idempotent&&failed;
            return ExecutionObservation{failed?ObservationState::Failed:ObservationState::CompletedCandidate,
                value,digest(value),tool_name_=="Wget"?r.input.value("output_path","")+".part":"",
                uncertain?"remote_effect_unknown":failed?"network_tool_failed":"",!uncertain,!failed};
        },e);
    }
    ReconciliationResult NetworkToolExecutionAdapter::reconcile(const ExecutionRequest&r,const ExecutionHandle&h)
    {
        auto observed=query(h);
        if(observed.state!=ObservationState::Unknown)return {observed,false};
        const bool safe=tool_name_=="Wget"&&r.invocation.idempotent&&r.input.value("resume",false);
        observed.error_code=safe?"download_checkpoint_restart_required":"remote_effect_unknown";
        observed.effect_known=safe;
        return {observed,safe};
    }
    BubblewrapExecutionAdapter::BubblewrapExecutionAdapter(std::shared_ptr<sandbox::SandboxProvider> p, sandbox::SandboxSpec s, std::string r, std::string g) : provider_(std::move(p)), spec_(std::move(s)), revision_(std::move(r)), generation_(std::move(g))
    {
        if (!provider_)
            throw std::invalid_argument("sandbox provider required");
    }
    std::optional<ExecutionHandle> BubblewrapExecutionAdapter::start(const ExecutionRequest &r, std::string *e)
    {
        auto spec = spec_;
        if (r.control.deadline_at_ms)
        {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            if (r.control.deadline_at_ms <= now)
            {
                if (e)
                    *e = "execution deadline exceeded";
                return {};
            }
            const auto remaining = static_cast<std::uint64_t>(r.control.deadline_at_ms - now);
            spec.wall_time_ms = spec.wall_time_ms ? std::min(spec.wall_time_ms, remaining) : remaining;
        }
        auto h = provider_->create(spec, e);
        if (!h)
            return {};
        auto launched = launch(r, [p = provider_, h = *h](auto c)
                               {if(c->load()){p->destroy(h,nullptr);return ExecutionObservation{ObservationState::Cancelled,{}, {},{}, {},false,true};}std::string error;auto v=p->exec(h,&error);p->destroy(h,nullptr);if(!v)return ExecutionObservation{ObservationState::Failed,{}, {},{},error,false,true};nlohmann::json result={{"exit_code",v->exit_code},{"stdout",v->stdout_text},{"stderr",v->stderr_text},{"manifest",sandbox::encode(v->manifest)}};return ExecutionObservation{v->timed_out||v->exit_code!=0?ObservationState::Failed:ObservationState::CompletedCandidate,result,digest(result),v->manifest.workspace_output_digest,v->timed_out?"deadline_exceeded":v->exit_code?"process_failed":"",true,true}; }, e);
        if (launched)
        {
            std::lock_guard l(handles_mutex_);
            handles_[launched->external_id] = *h;
        }
        return launched;
    }
    CancellationResult BubblewrapExecutionAdapter::escalate(const ExecutionHandle &h, CancellationStage s)
    {
        sandbox::SandboxHandle handle;
        {
            std::lock_guard l(handles_mutex_);
            auto it = handles_.find(h.external_id);
            if (it == handles_.end())
                return AsyncExecutionAdapter::escalate(h, s);
            handle = it->second;
        }
        auto signal = s == CancellationStage::Kill ? sandbox::SandboxSignal::Kill : s == CancellationStage::Terminate ? sandbox::SandboxSignal::Terminate
                                                                                                                      : sandbox::SandboxSignal::Cooperative;
        auto result = provider_->cancel(handle, signal);
        return {result.accepted, result.terminal, result.diagnostic, result.effect_known, result.receipt_digest};
    }
    ChildTaskExecutionAdapter::ChildTaskExecutionAdapter(std::shared_ptr<ChildTaskBackend> b, ChildTaskRequest r, std::string i, std::string v, std::string g, bool remote) : backend_(std::move(b)), request_(std::move(r)), id_(std::move(i)), revision_(std::move(v)), generation_(std::move(g)), remote_(remote)
    {
        if (!backend_)
            throw std::invalid_argument("child backend required");
    }
    std::optional<ExecutionHandle> ChildTaskExecutionAdapter::start(const ExecutionRequest &r, std::string *e)
    {
        auto req = request_;
        req.inputs = r.input;
        req.idempotency_key = r.idempotency_key;
        if (r.control.deadline_at_ms)
        {
            const auto now_system = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            if (r.control.deadline_at_ms <= now_system)
            {
                if (e)
                    *e = "execution deadline exceeded";
                return {};
            }
            req.policy.deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(r.control.deadline_at_ms - now_system);
        }
        return launch(r, [b = backend_, req = std::move(req)](auto c) mutable
                      {req.policy.cancel_requested=c;auto h=b->start(std::move(req));auto v=h->wait();auto value=child_task_result_to_json(v);auto state=v.status==ChildTaskStatus::Completed?ObservationState::CompletedCandidate:v.status==ChildTaskStatus::Cancelled?ObservationState::Cancelled:ObservationState::Failed;return ExecutionObservation{state,value,digest(value),v.checkpoint.dump(),v.error_code,v.verified_complete(),true}; }, e);
    }
    HTTPExecutionAdapter::HTTPExecutionAdapter(std::string i, std::string r, std::string g, Start s, Query q, Cancel c, Reconcile x) : id_(std::move(i)), revision_(std::move(r)), generation_(std::move(g)), start_(std::move(s)), query_(std::move(q)), cancel_(std::move(c)), reconcile_(std::move(x))
    {
        if (!start_ || !query_)
            throw std::invalid_argument("HTTP start/query protocol required");
    }
    std::optional<ExecutionHandle> HTTPExecutionAdapter::start(const ExecutionRequest &r, std::string *e)
    {
        auto id = start_(r, e);
        return id ? std::optional<ExecutionHandle>{{id_, revision_, generation_, *id, r.fencing_token}} : std::nullopt;
    }
    std::optional<ExecutionHandle> HTTPExecutionAdapter::attach(const ExecutionRequest &r, std::string *e)
    {
        if (r.checkpoint_ref.empty())
        {
            if (e)
                *e = "external operation id required for attach";
            return {};
        }
        return ExecutionHandle{id_, revision_, generation_, r.checkpoint_ref, r.fencing_token};
    }
    ExecutionObservation HTTPExecutionAdapter::query(const ExecutionHandle &h) { return query_(h.external_id); }
    CancellationResult HTTPExecutionAdapter::cancel(const ExecutionHandle &h) { return cancel_ ? cancel_(h.external_id) : CancellationResult{false, false, "cancel endpoint unavailable"}; }
    ReconciliationResult HTTPExecutionAdapter::reconcile(const ExecutionRequest &r, const ExecutionHandle &h) { return reconcile_ ? reconcile_(r, h.external_id) : ReconciliationResult{query(h), false}; }
    RemoteProtocolExecutionAdapter::RemoteProtocolExecutionAdapter(std::shared_ptr<RemoteExecutionProtocol> p, ExecutionAdapterKind k, std::string i, std::string r, std::string g, RestartPolicy restart) : protocol_(std::move(p)), kind_(k), id_(std::move(i)), revision_(std::move(r)), generation_(std::move(g)), restart_(restart)
    {
        if (!protocol_ || (k != ExecutionAdapterKind::MCP && k != ExecutionAdapterKind::A2AChildTask))
            throw std::invalid_argument("native MCP/A2A protocol required");
    }
    RemoteExecutionIdentity RemoteProtocolExecutionAdapter::identity(const ExecutionHandle &h) const { return {h.external_id, h.provider_session_id, h.remote_task_id, h.peer_id}; }
    std::optional<ExecutionHandle> RemoteProtocolExecutionAdapter::start(const ExecutionRequest &r, std::string *e)
    {
        auto x = protocol_->start(r, e);
        return x ? std::optional<ExecutionHandle>{{id_, revision_, generation_, x->external_id, r.fencing_token, x->session_id, x->task_id, x->peer_id}} : std::nullopt;
    }
    std::optional<ExecutionHandle> RemoteProtocolExecutionAdapter::attach(const ExecutionRequest &r, std::string *e)
    {
        RemoteExecutionIdentity x{r.invocation.external_operation_id, r.invocation.provider_session_id, r.invocation.remote_task_id, r.invocation.adapter_peer_id};
        if (x.external_id.empty() || x.session_id.empty() || (kind_ == ExecutionAdapterKind::A2AChildTask && (x.task_id.empty() || x.peer_id.empty())))
        {
            if (e)
                *e = "pinned remote execution identity required";
            return {};
        }
        if (!protocol_->attach(x, e))
            return {};
        return ExecutionHandle{id_, revision_, generation_, x.external_id, r.fencing_token, x.session_id, x.task_id, x.peer_id};
    }
    ExecutionObservation RemoteProtocolExecutionAdapter::query(const ExecutionHandle &h) { return protocol_->query(identity(h)); }
    CancellationResult RemoteProtocolExecutionAdapter::cancel(const ExecutionHandle &h) { return protocol_->cancel(identity(h), CancellationStage::Cooperative); }
    CancellationResult RemoteProtocolExecutionAdapter::escalate(const ExecutionHandle &h, CancellationStage s) { return protocol_->cancel(identity(h), s); }
    ReconciliationResult RemoteProtocolExecutionAdapter::reconcile(const ExecutionRequest &r, const ExecutionHandle &h) { return protocol_->reconcile(r, identity(h)); }
}
