#include <cassert>
#include <filesystem>

#include "agent/internal/platform_io.hpp"
#include "agent/sandbox/harness_port.hpp"

namespace {
class CountingProvider final : public agent_framework::sandbox::SandboxProvider {
public:
    std::string id() const override { return "counting"; }
    std::string version() const override { return "v1"; }
    bool available(std::string*) const override { return true; }
    std::optional<agent_framework::sandbox::SandboxHandle> create(
        const agent_framework::sandbox::SandboxSpec& spec, std::string*) override {
        ++creates; return agent_framework::sandbox::SandboxHandle{"h", agent_framework::sandbox::encode(spec).at("canonical_digest")};
    }
    std::optional<agent_framework::sandbox::ExecResult> exec(
        const agent_framework::sandbox::SandboxHandle& handle, std::string*) override {
        ++executes; agent_framework::sandbox::ExecResult out; out.exit_code=0;
        out.manifest.metadata=metadata; out.manifest.sandbox_id=handle.sandbox_id;
        out.manifest.spec_digest=handle.spec_digest; out.manifest.provider_version="v1";
        out.manifest.workspace_input_digest="sha256:input";out.manifest.workspace_output_digest="sha256:output";
        out.manifest.stdout_digest="sha256:stdout";out.manifest.stderr_digest="sha256:stderr";
        out.manifest.started_at="1";out.manifest.finished_at="2";return out;
    }
    bool destroy(const agent_framework::sandbox::SandboxHandle&,std::string*)override{++destroys;return true;}
    agent_framework::contracts::ContractMetadata metadata; int creates{0},executes{0},destroys{0};
};
}
int main(){using namespace agent_framework;const auto path=(std::filesystem::temp_directory_path()/
    ("phase4-sandbox-journal-"+std::to_string(internal::current_process_id())+".sqlite3"));std::error_code ec;std::filesystem::remove(path,ec);
    CountingProvider provider;sandbox::SandboxSpec spec;spec.provider="counting";spec.workspace_base_digest="sha256:input";spec.command={"true"};spec.cpu_millis=1;spec.memory_bytes=1;spec.wall_time_ms=1;spec.policy_revision="p";spec.memory_view_digest="sha256:view";
    harness::HarnessStageRequest request;request.checkpoint.metadata.identity.tenant_id="tenant";request.checkpoint.metadata.identity.task_id="task";request.checkpoint.metadata.identity.run_id="run";provider.metadata=request.checkpoint.metadata;request.stage=harness::HarnessStage::Execution;request.idempotency_key="effect-1";
    {sandbox::SQLiteSandboxReceiptJournal journal(path.string());sandbox::SandboxExecutionHarnessPort port("sandbox",provider,spec,journal);auto first=port.execute(request);assert(first.outcome==harness::StageOutcome::Succeeded&&first.pins.artifact_manifest_digest=="sha256:output");auto replay=port.execute(request);assert(replay.outcome==harness::StageOutcome::Succeeded);assert(provider.executes==1);}
    {sandbox::SQLiteSandboxReceiptJournal journal(path.string());sandbox::SandboxExecutionHarnessPort port("sandbox",provider,spec,journal);auto replay=port.reconcile(request);assert(replay&&replay->effect_receipt_digest.size()>10);assert(provider.executes==1);}
    std::filesystem::remove(path,ec);return 0;}
