#include <agent/tool_runtime/typed_adapters.hpp>
#include <agent/tool_runtime/commit_coordinator.hpp>
#include <agent/tool_runtime/worker_runtime.hpp>
#include <agent/internal/platform_io.hpp>
#include <cassert>
#include <filesystem>
#include <thread>
using namespace agent_framework;
using namespace agent_framework::tool_runtime;
namespace
{
    class Participant final : public harness::CrossStoreParticipant
    {
    public:
        std::string id() const override { return "pins"; }
        std::string capability_manifest_digest() const override { return "sha256:pins"; }
        bool supports(const harness::CrossStoreOperation &) const noexcept override { return true; }
        std::optional<harness::ParticipantPin> inspect(const harness::CrossStoreOperation &, std::string *) override { return harness::ParticipantPin{id(), 1, "sha256:pin", false}; }
        bool prepare(const harness::CrossStoreOperation &, const harness::ParticipantPin &, std::string *) override { return true; }
        bool commit(const harness::CrossStoreOperation &, const harness::ParticipantPin &, std::string *) override { return true; }
        bool confirm(const harness::CrossStoreOperation &, const harness::ParticipantPin &, std::string *) override { return true; }
        bool compensate(const harness::CrossStoreOperation &, const harness::ParticipantPin &, std::string *) override { return false; }
    };
    LongRunningToolInvocation value(std::string id, bool idem = true)
    {
        LongRunningToolInvocation v;
        v.metadata.identity.tenant_id = "tenant";
        v.metadata.identity.task_id = "task";
        v.metadata.identity.run_id = "run";
        v.invocation_id = id;
        v.conversation_id = "conversation";
        v.turn_id = "turn";
        v.tool_call_id = id + "-call";
        v.tool_name = "echo";
        v.tool_contract_revision = "v1";
        v.deployment_revision = "d1";
        v.tool_generation = "g1";
        v.input_digest = "sha256:input";
        v.created_at = v.updated_at = "now";
        v.idempotent = idem;
        return v;
    }
}
int main()
{
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / ("execution-adapters-" + std::to_string(internal::current_process_id()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    ExecutionAdapterRegistry production;
    std::string error;
    CallbackAdapterSpec bad{"bad", "v1", "g1", ExecutionAdapterKind::LocalTool, AdapterOrigin::Scripted, {}, RestartPolicy::ManualReview};
    auto scripted = std::make_shared<CallbackExecutionAdapter>(bad, [](const ExecutionRequest &r, std::string *)
                                                               { return ExecutionHandle{"bad", "v1", "g1", "x", r.fencing_token}; }, [](const ExecutionHandle &)
                                                               { return ExecutionObservation{ObservationState::Running}; });
    assert(!production.register_adapter(scripted, &error));
    auto bus = std::make_shared<ToolBus>();
    ToolMeta meta;
    meta.name = "echo";
    meta.description = "echo";
    meta.schema = {{"type", "object"}, {"properties", {{"ok", {{"type", "boolean"}}}}}, {"required", {"ok"}}, {"additionalProperties", false}};
    bus->register_cancellable_local_tool("echo", [](const json &v, const ToolCallControl &c)
                                         { return c.should_stop() ? json{{"code", "cancelled"}} : v; }, meta);
    auto local = std::make_shared<ToolBusExecutionAdapter>(bus, "local", "v1", "g1");
    assert(production.register_adapter(local));
    ExecutionRequest req{value("adapter"), {{"ok", true}}, "adapter", {}, 7};
    auto h = local->start(req, &error);
    assert(h);
    ExecutionObservation obs;
    for (int i = 0; i < 100; ++i)
    {
        obs = local->query(*h);
        if (obs.state != ObservationState::Running)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(obs.state == ObservationState::CompletedCandidate && obs.result.at("ok"));
    std::map<std::string, ExecutionObservation> remote;
    auto http = std::make_shared<HTTPExecutionAdapter>("http", "v1", "g1", [&](const ExecutionRequest &, std::string *)
                                                       {remote["op"]={ObservationState::Running};return std::optional<std::string>{"op"}; }, [&](std::string_view id)
                                                       { return remote[std::string(id)]; });
    assert(production.register_adapter(http));
    auto hh = http->start(req, &error);
    assert(hh);
    req.checkpoint_ref = "op";
    auto attached = http->attach(req, &error);
    assert(attached && attached->external_id == "op");
    auto path = (root / "control.sqlite3").string();
    SQLiteInvocationStore inv(path);
    SQLiteExecutionControlStore controls(path);
    distributed::SQLiteDurableQueue queue(path);
    distributed::SQLiteWorkerRegistry workers(path);
    auto gen = workers.register_worker({"w", "i", "sha256:c", 0, 0});
    assert(gen && workers.set_quota("tenant", 1));
    LeaseWorkerRuntime runtime(inv, queue, workers, {"w", "i", *gen}, 100, &controls);
    auto v = value("commit");
    v.budget = {60000, 2, 4096};
    assert(runtime.enqueue(v));
    auto durable_control = controls.load("commit");
    assert(durable_control && durable_control->control.deadline_at_ms > 0 &&
           durable_control->control.backpressure.maximum_pending_events == 2);
    auto claim = runtime.claim("tenant", 10);
    assert(claim);
    auto durable_handle = runtime.start(*claim, *http, {{"request", true}}, &error);
    assert(durable_handle && durable_handle->external_id == "op");
    auto pinned = inv.load("commit");
    assert(pinned && pinned->adapter_id == "http" && pinned->adapter_revision == "v1" &&
           pinned->adapter_generation == "g1" && pinned->external_operation_id == "op" &&
           pinned->adapter_restart_policy == "attach");
    auto recovered = runtime.recover(*claim, production, {{"request", true}}, &error);
    assert(recovered && recovered->external_id == "op");
    InvocationReceipt receipt{"sha256:candidate", "", "", "now", true};
    assert(runtime.complete(*claim, receipt, 20));
    run::SQLiteRunStore runs((root / "run.sqlite3").string());
    run::RunCheckpoint cp;
    cp.metadata = v.metadata;
    cp.state = run::RunState::Running;
    assert(runs.create(cp));
    auto loaded = runs.load("run");
    run::RunCommit rc{loaded->checkpoint, loaded->revision, "effect_prepared", {}, run::EffectRecord{"commit", "commit-key", run::EffectState::Prepared, "sha256:req", "", claim->queue_lease.fencing_token}, {}};
    assert(runs.commit(rc));
    distributed::FilesystemObjectStore objects(root / "objects");
    ToolEffectJournal effects(root / "effects.wal");
    harness::SQLiteCoordinationJournal journal((root / "coord.sqlite3").string());
    harness::CrossStoreCoordinator cross(journal, "invocation-commit-v1");
    assert(cross.register_participant(std::make_shared<Participant>()));
    InvocationCommitCoordinator coordinator(inv, objects, effects, runs, cross);
    auto committed = coordinator.commit({"commit", "tenant", "run", "harness", "commit-key", "sha256:req", "application/json", {{"answer", 42}}, claim->queue_lease.fencing_token, true, true});
    if (!committed.committed)
        throw std::runtime_error(committed.error);
    assert(!committed.artifact_digest.empty());
    assert(inv.load("commit")->state == InvocationState::EffectCommitted);
    assert(runs.effect("run", "commit")->state == run::EffectState::Committed);

    // Durable cancellation is generation-scoped and unsupported force-kill fails closed
    // through reconciliation into ManualReview instead of reporting false success.
    CallbackAdapterSpec cancellation_spec{"cancel-test", "v1", "g1", ExecutionAdapterKind::HTTP, AdapterOrigin::Test, {true, true, true, false, true, true, true}, RestartPolicy::Attach};
    auto cancellation_adapter = std::make_shared<CallbackExecutionAdapter>(cancellation_spec, [](const ExecutionRequest &r, std::string *)
                                                                           { return std::optional<ExecutionHandle>{{"cancel-test", "v1", "g1", "remote-op", r.fencing_token}}; }, [](const ExecutionHandle &)
                                                                           { return ExecutionObservation{ObservationState::Unknown, {}, {}, {}, "remote_unreachable", false, false}; }, [](const ExecutionHandle &)
                                                                           { return CancellationResult{true, false, "cancel accepted", false, {}}; }, [](const ExecutionRequest &, const ExecutionHandle &)
                                                                           { return ReconciliationResult{{ObservationState::Unknown, {}, {}, {}, "effect_unknown", false, false}, false}; });
    auto cv = value("cancelled-effect");
    assert(runtime.enqueue(cv));
    auto cancel_claim = runtime.claim("tenant", 30);
    assert(cancel_claim);
    auto cancel_handle = runtime.start(*cancel_claim, *cancellation_adapter, {}, &error);
    assert(cancel_handle);
    assert(runtime.request_cancel(*cancel_claim, controls, "user_interrupt", 31, &error));
    assert(runtime.drive_cancel(*cancel_claim, *cancellation_adapter, *cancel_handle, controls, 31, 10, &error));
    assert(controls.load("cancelled-effect")->stage == CancellationStage::Cooperative);
    assert(runtime.drive_cancel(*cancel_claim, *cancellation_adapter, *cancel_handle, controls, 41, 10, &error));
    assert(controls.load("cancelled-effect")->stage == CancellationStage::Reconciling);
    assert(runtime.drive_cancel(*cancel_claim, *cancellation_adapter, *cancel_handle, controls, 51, 10, &error));
    assert(controls.load("cancelled-effect")->stage == CancellationStage::ManualReview);
    assert(inv.load("cancelled-effect")->state == InvocationState::ManualReview);

    auto unknown = value("unknown", false);
    unknown.lease = {"w", "i", *gen, 9, 100};
    assert(inv.create(unknown));
    auto p = unknown;
    auto move = [&](InvocationState state, const char *type)
    {auto prior=p.revision;p.revision++;p.state=state;InvocationEvent e;e.event_type=type;e.fencing_token=9;assert(inv.commit({p,prior,e,{},{},{}})); };
    move(InvocationState::Admitted, "admit");
    move(InvocationState::Queued, "queue");
    move(InvocationState::Leased, "lease");
    move(InvocationState::Running, "run");
    move(InvocationState::Reconciling, "unknown");
    auto manual = coordinator.commit({"unknown", "tenant", "run", "harness", "unknown-key", "sha256:req", "application/json", {}, 9, false, false});
    assert(manual.manual_review && inv.load("unknown")->state == InvocationState::ManualReview);
    fs::remove_all(root, ec);
}
