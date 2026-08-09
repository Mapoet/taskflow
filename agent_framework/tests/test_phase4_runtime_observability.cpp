#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>

#include "agent/internal/platform_io.hpp"
#include "agent/sandbox/provider.hpp"
#include "agent/sandbox/workspace.hpp"
#include "agent/telemetry/runtime.hpp"

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
    telemetry::TelemetryRuntime runtime(sink, {{"run.state", "provider.id"}, 64});
    telemetry::SpanRecord span;
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
    metric.metric_name = "sandbox.wall_time";
    metric.value = 1.0;
    metric.unit = "ms";
    assert(runtime.emit_metric(metric));
    assert(runtime.dropped() == 1);
    assert(sink->spans().size() == 1 && sink->metrics().size() == 1);
    std::filesystem::remove_all(root, ec);
    return 0;
}
