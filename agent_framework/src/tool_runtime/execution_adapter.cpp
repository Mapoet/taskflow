#include "agent/tool_runtime/execution_adapter.hpp"
#include <stdexcept>
namespace agent_framework::tool_runtime
{
    std::optional<ExecutionHandle> ExecutionAdapter::attach(const ExecutionRequest &, std::string *e)
    {
        if (e)
            *e = "adapter does not support attach";
        return {};
    }
    bool ExecutionAdapterRegistry::register_adapter(std::shared_ptr<ExecutionAdapter> a, std::string *e)
    {
        if (!a || a->id().empty() || a->revision().empty() || a->deployment_generation().empty())
        {
            if (e)
                *e = "adapter identity incomplete";
            return false;
        }
        if (production_ && a->origin() != AdapterOrigin::Production)
        {
            if (e)
                *e = "test/scripted adapter rejected by production registry";
            return false;
        }
        auto c = a->capabilities();
        if (c.attach && a->restart_policy() != RestartPolicy::Attach)
        {
            if (e)
                *e = "attach capability requires attach restart policy";
            return false;
        }
        if (!c.attach && a->restart_policy() == RestartPolicy::Attach)
        {
            if (e)
                *e = "attach restart policy requires attach capability";
            return false;
        }
        const auto key = a->id() + "\x1f" + a->revision() + "\x1f" + a->deployment_generation();
        std::lock_guard l(mutex_);
        if (adapters_.count(key))
        {
            if (e)
                *e = "adapter already registered";
            return false;
        }
        adapters_[key] = std::move(a);
        return true;
    }
    std::shared_ptr<ExecutionAdapter> ExecutionAdapterRegistry::find(std::string_view i, std::string_view r, std::string_view g) const
    {
        std::lock_guard l(mutex_);
        auto it = adapters_.find(std::string(i) + "\x1f" + std::string(r) + "\x1f" + std::string(g));
        return it == adapters_.end() ? nullptr : it->second;
    }
    CallbackExecutionAdapter::CallbackExecutionAdapter(CallbackAdapterSpec s, AdapterStartFn start, AdapterQueryFn query, AdapterCancelFn cancel, AdapterReconcileFn reconcile) : spec_(std::move(s)), start_(std::move(start)), query_(std::move(query)), cancel_(std::move(cancel)), reconcile_(std::move(reconcile))
    {
        if (!start_ || !query_)
            throw std::invalid_argument("adapter start/query callbacks required");
    }
    std::optional<ExecutionHandle> CallbackExecutionAdapter::start(const ExecutionRequest &r, std::string *e)
    {
        auto h = start_(r, e);
        if (h)
        {
            h->adapter_id = spec_.id;
            h->adapter_revision = spec_.revision;
            h->deployment_generation = spec_.generation;
            h->fencing_token = r.fencing_token;
        }
        return h;
    }
    std::optional<ExecutionHandle> CallbackExecutionAdapter::attach(const ExecutionRequest &r, std::string *e)
    {
        if (!spec_.capabilities.attach)
            return ExecutionAdapter::attach(r, e);
        return start(r, e);
    }
    ExecutionObservation CallbackExecutionAdapter::query(const ExecutionHandle &h) { return query_(h); }
    CancellationResult CallbackExecutionAdapter::cancel(const ExecutionHandle &h) { return cancel_ ? cancel_(h) : CancellationResult{false, false, "cancel unsupported"}; }
    ReconciliationResult CallbackExecutionAdapter::reconcile(const ExecutionRequest &r, const ExecutionHandle &h) { return reconcile_ ? reconcile_(r, h) : ReconciliationResult{{ObservationState::Unknown, {}, {}, {}, "reconcile_unsupported", false, false}, false}; }
}
