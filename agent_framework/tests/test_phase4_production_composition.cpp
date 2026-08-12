#include <cassert>
#include <filesystem>

#include "agent/harness/production_composition.hpp"
#include "agent/internal/platform_io.hpp"
#include "phase4_harness_test_support.hpp"

int main() {
    using namespace agent_framework;
    using namespace agent_framework::harness;
    using namespace phase4_harness_test;
    const auto root = std::filesystem::temp_directory_path() /
        ("phase4-production-composition-" + std::to_string(internal::current_process_id()));
    std::error_code ec; std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    SQLiteHarnessStore harnesses((root / "harness.sqlite3").string());
    run::SQLiteRunStore runs((root / "run.sqlite3").string());
    Phase4ProductionComposition composition(harnesses, runs);
    const auto empty = composition.validate();
    assert(!empty.ready && empty.issues.size() == 11);

    for(std::size_t index = 0; index < static_cast<std::size_t>(HarnessStage::Complete); ++index) {
        const auto stage = static_cast<HarnessStage>(index);
        const bool side_effecting = stage == HarnessStage::Execution ||
                                    stage == HarnessStage::Reexecution;
        auto execute = [](const HarnessStageRequest& request) {
            auto result = successful(request, false);
            if(request.stage == HarnessStage::Assurance ||
               request.stage == HarnessStage::Reverification) {
                result.acceptance_decision = "accepted";
                result.pins.acceptance_report_digest = "sha256:acceptance";
            }
            return result;
        };
        ManifestHarnessStagePort::Reconcile reconcile;
        if(side_effecting) reconcile = [execute](const HarnessStageRequest& request) {
            return std::optional<HarnessStageResult>(execute(request));
        };
        assert(composition.bind(stage, std::make_shared<ManifestHarnessStagePort>(
            "production-" + harness_stage_name(stage), side_effecting,
            "test-production-adapter", "sha256:manifest-" + harness_stage_name(stage),
            execute, reconcile)));
    }
    const auto ready = composition.validate();
    assert(ready.ready && !ready.manifest_digest.empty());
    std::string error;
    auto runtime = composition.build(&error);
    assert(runtime && error.empty());
    std::filesystem::remove_all(root, ec);
    return 0;
}
