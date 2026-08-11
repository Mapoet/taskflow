#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>

#include "agent/internal/platform_io.hpp"
#include "agent/sandbox/provider.hpp"
#include "agent/sandbox/credential_broker.hpp"
#include "agent/sandbox/workspace.hpp"
#include "agent/telemetry/runtime.hpp"
#include "agent/telemetry/audit_bridge.hpp"
#include "agent/telemetry/durable_spool.hpp"
#include "agent/telemetry/slo.hpp"
#include "agent/telemetry/trace_context.hpp"

namespace {
class FakeSandbox final : public agent_framework::sandbox::SandboxProvider {
public:
    std::string id() const override { return "fake"; }
    std::string version() const override { return "v1"; }
    bool available(std::string*) const override { return true; }
    std::optional<agent_framework::sandbox::SandboxHandle> create(
        const agent_framework::sandbox::SandboxSpec& spec, std::string* error) override {
        const auto issues = agent_framework::sandbox::validate(spec);
        if(!issues.empty()) { if(error) *error = issues.front().code; return std::nullopt; }
        return agent_framework::sandbox::SandboxHandle{
            "sandbox-a", agent_framework::sandbox::encode(spec).at("canonical_digest")};
    }
    std::optional<agent_framework::sandbox::ExecResult> exec(
        const agent_framework::sandbox::SandboxHandle& handle, std::string*) override {
        agent_framework::sandbox::ExecResult result;
        result.exit_code = 0;
        result.stdout_text = "ok";
        result.manifest.sandbox_id = handle.sandbox_id;
        result.manifest.spec_digest = handle.spec_digest;
        return result;
    }
    bool destroy(const agent_framework::sandbox::SandboxHandle&, std::string*) override { return true; }
};
class ToggleSink final : public agent_framework::telemetry::TelemetrySink {
public:
    bool export_span(const agent_framework::telemetry::SpanRecord&) override { ++spans; return accept; }
    bool export_metric(const agent_framework::telemetry::MetricResult&) override { ++metrics; return accept; }
    bool flush() override { return accept; }
    bool accept{false}; int spans{0}; int metrics{0};
};
}

int main() {
    using namespace agent_framework;
    const auto root = std::filesystem::temp_directory_path() /
        ("agent-phase4-workspace-" + std::to_string(internal::current_process_id()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "src");
    std::ofstream(root / "src" / "main.cpp") << "int main(){}";
    auto first = sandbox::snapshot_workspace(root, 1024);
    auto second = sandbox::snapshot_workspace(root, 1024);
    assert(first && second && first->digest == second->digest);
    assert(!sandbox::snapshot_workspace(root, 1));

    sandbox::SandboxSpec spec;
    spec.metadata.identity.tenant_id = "tenant-a";
    spec.metadata.identity.task_id = "task-a";
    spec.metadata.identity.run_id = "run-a";
    spec.provider = "fake";
    spec.workspace_base_digest = first->digest;
    spec.command = {"/bin/true"};
    spec.read_only_mounts = {"/usr"};
    spec.cpu_millis = 1000;
    spec.memory_bytes = 64 * 1024 * 1024;
    spec.wall_time_ms = 5000;
    spec.policy_revision = "policy-v1";
    spec.memory_view_digest = "sha256:view";
    assert(sandbox::validate(spec).empty());
    spec.credential_refs = {"password=raw"};
    assert(!sandbox::validate(spec).empty());
    spec.credential_refs = {"vault://tenant/credential"};
    sandbox::SandboxProviderRegistry registry;
    assert(registry.register_provider(std::make_shared<FakeSandbox>()));
    auto handle = registry.find("fake")->create(spec);
    assert(handle);
    assert(registry.find("fake")->exec(*handle)->exit_code == 0);

    auto sink = std::make_shared<telemetry::InMemoryTelemetrySink>();
    auto audit = std::make_shared<TestAuditSink>();
    auto bridged = std::make_shared<telemetry::AuditBridgeTelemetrySink>(sink, audit);
    telemetry::TelemetryRuntime runtime(bridged, {{"run.state", "provider.id"}, 64});
    telemetry::SpanRecord span;
    span.context.metadata.identity.tenant_id = "tenant-a";
    span.context.metadata.identity.task_id = "task-a";
    span.context.metadata.identity.run_id = "run-a";
    span.context.trace_id = "trace-a";
    span.context.span_id = "span-a";
    span.name = "sandbox.exec";
    span.status = "ok";
    span.attributes = {{"run.state", "running"}, {"provider.id", "fake"}};
    assert(runtime.emit_span(span));
    span.attributes["prompt"] = "secret prompt";
    assert(!runtime.emit_span(span));
    telemetry::MetricResult metric;
    metric.metadata.identity.tenant_id = "tenant-a";
    metric.metadata.identity.task_id = "task-a";
    metric.metadata.identity.run_id = "run-a";
    metric.metric_name = "sandbox.wall_time";
    metric.value = 1.0;
    metric.unit = "ms";
    assert(runtime.emit_metric(metric));
    assert(runtime.dropped() == 1);
    assert(sink->spans().size() == 1 && sink->metrics().size() == 1);
    assert(audit->events().size() == 2);
    assert(audit->events().front().trace_id == "trace-a");

    const auto trace = telemetry::parse_traceparent(
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01", "vendor=value");
    assert(trace && trace->sampled && telemetry::format_traceparent(*trace) ==
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
    assert(!telemetry::parse_traceparent(
        "00-00000000000000000000000000000000-00f067aa0ba902b7-01"));
    assert(!telemetry::parse_traceparent(
        "00-4BF92F3577B34DA6A3CE929D0E0E4736-00f067aa0ba902b7-01"));

    sandbox::CredentialBroker broker([](std::string_view reference)
        -> std::optional<sandbox::CredentialLease> {
        if (reference != "vault://tenant/key") return std::nullopt;
        return sandbox::CredentialLease{std::string(reference), "never-persist-this", "2099-01-01T00:00:00Z"};
    });
    std::string broker_error;
    auto leases = broker.resolve({"vault://tenant/key"}, &broker_error);
    assert(leases && leases->size() == 1);
    std::string diagnostic = "failure never-persist-this";
    sandbox::CredentialBroker::redact(diagnostic, *leases);
    assert(diagnostic == "failure [REDACTED]");
    assert(!broker.resolve({"password=raw"}, &broker_error));
    assert(!broker.resolve({"vault://tenant/key", "vault://tenant/key"}, &broker_error));

    telemetry::SloRegistry slos;
    assert(slos.register_objective({"latency", "sandbox.wall_time",
        telemetry::SloComparator::LessEqual, 100.0, 2, true}));
    auto insufficient = slos.evaluate({metric});
    assert(!insufficient.allowed && insufficient.blockers.front() == "slo:latency:inconclusive");
    metric.value = 90.0; metric.sample_count = 2;
    assert(slos.evaluate({metric}).allowed);
    metric.value = 110.0;
    auto violation = slos.evaluate({metric});
    assert(!violation.allowed && violation.evaluations.front().outcome == "failed");

    const auto spool_path = root / "telemetry-spool.sqlite3";
    span.attributes.erase("prompt");
    auto toggle = std::make_shared<ToggleSink>();
    {
        telemetry::SQLiteTelemetrySpool spool(spool_path.string(), toggle);
        assert(spool.export_span(span)); assert(spool.export_metric(metric));
        assert(spool.pending() == 2 && !spool.flush() && spool.pending() == 2);
    }
    toggle->accept = true;
    {
        telemetry::SQLiteTelemetrySpool recovered(spool_path.string(), toggle);
        assert(recovered.pending() == 2 && recovered.flush() && recovered.pending() == 0);
    }
    std::filesystem::remove_all(root, ec);
    return 0;
}
