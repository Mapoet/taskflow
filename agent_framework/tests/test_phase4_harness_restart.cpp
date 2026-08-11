#include <cassert>
#include <filesystem>
#include <stdexcept>

#include "agent/harness/runtime.hpp"
#include "agent/internal/platform_io.hpp"
#include "phase4_harness_test_support.hpp"

int main() {
    using namespace agent_framework;
    using namespace agent_framework::harness;
    using namespace phase4_harness_test;

    const auto root = std::filesystem::temp_directory_path() /
        ("phase4-harness-restart-" + std::to_string(internal::current_process_id()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    auto counters = std::make_shared<PortCounters>();
    {
        SQLiteHarnessStore store((root / "recoverable.sqlite3").string());
        Phase4HarnessRuntime runtime(store, ports(counters));
        HarnessRuntimeOptions options;
        options.now = [] { return "2026-08-11T00:00:00Z"; };
        options.after_stage_effect = [](HarnessStage stage) {
            if(stage == HarnessStage::Execution) throw std::runtime_error("simulated process death");
        };
        bool crashed = false;
        try { (void)runtime.run(start("harness-restart"), options); }
        catch(const std::runtime_error&) { crashed = true; }
        assert(crashed);
        assert(counters->execute[HarnessStage::Execution] == 1);
    }
    {
        SQLiteHarnessStore recovered((root / "recoverable.sqlite3").string());
        Phase4HarnessRuntime runtime(recovered, ports(counters));
        HarnessRuntimeOptions options;
        options.now = [] { return "2026-08-11T00:00:01Z"; };
        const auto result = runtime.resume("tenant-a", "harness-restart", options);
        assert(result.state == HarnessState::Completed);
        assert(counters->execute[HarnessStage::Execution] == 1);
        assert(counters->reconcile[HarnessStage::Execution] == 1);
    }

    auto unknown_counters = std::make_shared<PortCounters>();
    {
        SQLiteHarnessStore store((root / "unknown.sqlite3").string());
        Phase4HarnessRuntime runtime(store, ports(unknown_counters, false, false));
        HarnessRuntimeOptions options;
        options.now = [] { return "2026-08-11T00:00:00Z"; };
        options.after_stage_effect = [](HarnessStage stage) {
            if(stage == HarnessStage::Execution) throw std::runtime_error("simulated process death");
        };
        try { (void)runtime.run(start("harness-unknown"), options); }
        catch(const std::runtime_error&) {}
    }
    {
        SQLiteHarnessStore recovered((root / "unknown.sqlite3").string());
        Phase4HarnessRuntime runtime(recovered, ports(unknown_counters, false, false));
        const auto result = runtime.resume("tenant-a", "harness-unknown");
        assert(result.state == HarnessState::ManualReview);
        assert(result.checkpoint.terminal_reason == "unknown_external_effect");
        assert(unknown_counters->execute[HarnessStage::Execution] == 1);
    }

    InMemoryHarnessStore missing_store;
    HarnessPortRegistry missing_ports;
    auto intake = std::make_shared<CallbackHarnessStagePort>(
        "intake-only", false, [](const HarnessStageRequest& request) {
            return successful(request, false);
        });
    assert(missing_ports.bind(HarnessStage::Intake, intake));
    Phase4HarnessRuntime incomplete(missing_store, std::move(missing_ports));
    const auto missing = incomplete.run(start("harness-missing"));
    assert(missing.state == HarnessState::ManualReview);
    assert(missing.checkpoint.terminal_reason == "required_stage_port_missing:cognition");

    std::filesystem::remove_all(root, error);
    return 0;
}
