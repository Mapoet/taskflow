#include <cassert>
#include <filesystem>
#include <memory>

#include "agent/execution/harness_ports.hpp"
#include "agent/internal/platform_io.hpp"
#include "phase4_harness_test_support.hpp"

int main() {
    using namespace agent_framework;
    using namespace agent_framework::execution;
    using namespace agent_framework::harness;
    using namespace phase4_harness_test;
    namespace fs = std::filesystem;

    const auto root = fs::temp_directory_path() /
        ("phase4-harness-artifact-" + std::to_string(internal::current_process_id()));
    std::error_code ec; fs::remove_all(root, ec); fs::create_directories(root);
    SQLiteHarnessStore harness_store((root / "harness.sqlite3").string());
    SQLiteArtifactJournal artifact_journal((root / "artifacts.sqlite3").string());
    WorkspaceArtifactExecutor executor(root / "workspace", &artifact_journal);
    FilesystemArtifactOracle oracle(root / "workspace");

    const std::string initial_key = "harness-artifact:execution:0:1";
    const std::string repair_key = "harness-artifact:reexecution:1:1";
    HarnessPortRegistry registry;
    for(std::size_t index = 0; index < static_cast<std::size_t>(HarnessStage::Complete); ++index) {
        const auto stage = static_cast<HarnessStage>(index);
        std::shared_ptr<HarnessStagePort> port;
        if(stage == HarnessStage::Execution) {
            port = std::make_shared<ArtifactExecutionHarnessPort>("artifact-execution", executor,
                ArtifactAction{"source", ArtifactActionKind::WriteText, "src/main.cpp",
                               "int main(){return 0;}\n", "ignored", true});
        } else if(stage == HarnessStage::Reexecution) {
            port = std::make_shared<ArtifactExecutionHarnessPort>("artifact-reexecution", executor,
                ArtifactAction{"repair", ArtifactActionKind::WriteText, "README.md",
                               "# Complete\n", "ignored", true}, &artifact_journal, initial_key);
        } else if(stage == HarnessStage::Assurance) {
            port = std::make_shared<ArtifactAssuranceHarnessPort>("artifact-assurance",
                artifact_journal, oracle, initial_key,
                std::vector<ArtifactRequirement>{{"source", "src/main.cpp", true, {}},
                                                  {"documentation", "README.md", true, {}}});
        } else if(stage == HarnessStage::Reverification) {
            port = std::make_shared<ArtifactAssuranceHarnessPort>("artifact-reverification",
                artifact_journal, oracle, repair_key,
                std::vector<ArtifactRequirement>{{"source", "src/main.cpp", true, {}},
                                                  {"documentation", "README.md", true, {}}});
        } else {
            port = std::make_shared<CallbackHarnessStagePort>("port-" + harness_stage_name(stage),
                false, [](const HarnessStageRequest& request) { return successful(request, false); });
        }
        assert(registry.bind(stage, std::move(port)));
    }
    Phase4HarnessRuntime runtime(harness_store, std::move(registry));
    auto spec = start("harness-artifact");
    const auto result = runtime.run(spec);
    assert(result.state == HarnessState::Completed);
    assert(result.checkpoint.remediation_cycle == 1);
    assert(result.checkpoint.unresolved_findings.empty());
    assert(result.checkpoint.pins.artifact_manifest_digest ==
           artifact_journal.load(repair_key)->receipt.manifest.manifest_digest);
    assert(Phase4HarnessRuntime::completion_gate_issues(result.checkpoint).empty());
    const auto initial = artifact_journal.load(initial_key);
    const auto repaired = artifact_journal.load(repair_key);
    assert(initial && repaired);
    assert(repaired->receipt.manifest.parent_manifest_digest == initial->receipt.manifest.manifest_digest);
    assert(repaired->receipt.manifest.manifest_digest != initial->receipt.manifest.manifest_digest);

    SQLiteHarnessStore failed_store((root / "failed-harness.sqlite3").string());
    SQLiteArtifactJournal failed_journal((root / "failed-artifacts.sqlite3").string());
    WorkspaceArtifactExecutor failed_executor(root / "failed-workspace", &failed_journal);
    FilesystemArtifactOracle failed_oracle(root / "failed-workspace");
    const std::string failed_initial_key = "harness-failed:execution:0:1";
    const std::string failed_repair_key = "harness-failed:reexecution:1:1";
    HarnessPortRegistry failed_registry;
    for(std::size_t index = 0; index < static_cast<std::size_t>(HarnessStage::Complete); ++index) {
        const auto stage = static_cast<HarnessStage>(index);
        std::shared_ptr<HarnessStagePort> port;
        if(stage == HarnessStage::Execution) {
            port = std::make_shared<ArtifactExecutionHarnessPort>("failed-execution", failed_executor,
                ArtifactAction{"source", ArtifactActionKind::WriteText, "src/main.cpp",
                               "int main(){return 0;}\n", "ignored", true});
        } else if(stage == HarnessStage::Reexecution) {
            port = std::make_shared<ArtifactExecutionHarnessPort>("failed-reexecution", failed_executor,
                ArtifactAction{"bad-repair", ArtifactActionKind::WriteText, "README.md", "",
                               "ignored", true}, &failed_journal, failed_initial_key);
        } else if(stage == HarnessStage::Assurance || stage == HarnessStage::Reverification) {
            port = std::make_shared<ArtifactAssuranceHarnessPort>("failed-assurance-" +
                harness_stage_name(stage), failed_journal, failed_oracle,
                stage == HarnessStage::Assurance ? failed_initial_key : failed_repair_key,
                std::vector<ArtifactRequirement>{{"source", "src/main.cpp", true, {}},
                                                  {"documentation", "README.md", true, {}}});
        } else {
            port = std::make_shared<CallbackHarnessStagePort>("failed-" + harness_stage_name(stage),
                false, [](const HarnessStageRequest& request) { return successful(request, false); });
        }
        assert(failed_registry.bind(stage, std::move(port)));
    }
    Phase4HarnessRuntime failed_runtime(failed_store, std::move(failed_registry));
    auto failed_spec = start("harness-failed");
    failed_spec.max_remediation_cycles = 1;
    const auto failed = failed_runtime.run(failed_spec);
    assert(failed.state == HarnessState::ManualReview);
    assert(failed.checkpoint.terminal_reason == "remediation_cycle_limit");
    assert(failed.checkpoint.unresolved_findings ==
           std::vector<std::string>{"artifact:documentation"});
    assert(!Phase4HarnessRuntime::completion_gate_issues(failed.checkpoint).empty());
    fs::remove_all(root, ec);
    return 0;
}
