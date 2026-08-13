#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace agent_framework::tool_runtime {
enum class RecoveryEvidenceLevel { Offline, ProcessLive, ProviderLive };
enum class RecoveryScenario {
    WorkerCrashTakeover, StaleCompletion, CancelBeforeStart, CancelCompleteRace,
    DeadlineResultRace, CancelRestartEscalation, ObjectCorruption, MissingObject,
    SqliteBusy, StorageFailure, SchemaMigration, ProviderDisconnect, ProviderReattach
};
struct RecoveryCell {
    RecoveryScenario scenario{RecoveryScenario::WorkerCrashTakeover};
    RecoveryEvidenceLevel required_level{RecoveryEvidenceLevel::Offline};
    bool required{true}, executed{false}, passed{false};
    std::string reason, evidence_digest;
    std::uint64_t operations{0}, failures{0};
};
struct RecoveryCertificationReport {
    std::string matrix_revision{"ltw9-v1"}, environment_digest;
    std::vector<RecoveryCell> cells;
    std::vector<std::string> blockers;
    bool certified{false};
};
using RecoveryScenarioExecutor = std::function<RecoveryCell(RecoveryScenario)>;
std::string_view name(RecoveryScenario);
std::string_view name(RecoveryEvidenceLevel);
std::vector<RecoveryScenario> mandatory_recovery_scenarios();
RecoveryCertificationReport certify_recovery(std::string environment_digest,
    RecoveryEvidenceLevel available_level, const RecoveryScenarioExecutor&);
nlohmann::json encode(const RecoveryCertificationReport&);

struct SoakOptions { std::uint64_t iterations{1000}, seed{1}; std::size_t concurrency{1}; };
struct SoakReport {
    std::uint64_t iterations{0}, operations{0}, recoveries{0}, failures{0}, seed{0};
    std::size_t concurrency{0}; std::string evidence_digest; bool passed{false};
};
using SoakOperation = std::function<bool(std::uint64_t iteration, std::uint64_t random_value)>;
SoakReport run_recovery_soak(const SoakOptions&, const SoakOperation&);
}
