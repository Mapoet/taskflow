#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/harness/runtime.hpp"
#include "agent/run/store.hpp"

namespace agent_framework::harness {

enum class RunBindingState { Prepared, Committed, Reconciled, ManualReview };

struct RunHarnessBinding {
    std::string binding_id;
    std::string tenant_id;
    std::string run_id;
    std::string harness_id;
    HarnessStage stage{HarnessStage::Intake};
    std::uint64_t run_revision{0};
    std::string run_digest;
    std::uint64_t harness_revision{0};
    std::string harness_digest;
    RunBindingState state{RunBindingState::Prepared};
    std::string diagnostic;
};

struct RunBindingResult {
    bool committed{false};
    RunBindingState state{RunBindingState::Prepared};
    std::string error;
};

class SQLiteRunHarnessSaga final : public HarnessCheckpointObserver {
public:
    SQLiteRunHarnessSaga(std::string journal_path, run::RunStore& runs,
                         HarnessStore& harnesses);
    ~SQLiteRunHarnessSaga();
    SQLiteRunHarnessSaga(const SQLiteRunHarnessSaga&) = delete;
    SQLiteRunHarnessSaga& operator=(const SQLiteRunHarnessSaga&) = delete;

    RunBindingResult prepare(const RunHarnessBinding& binding);
    RunBindingResult commit(std::string_view binding_id);
    RunBindingResult reconcile(std::string_view binding_id);
    std::optional<RunHarnessBinding> load(std::string_view binding_id);
    std::vector<RunHarnessBinding> list_unresolved(std::size_t limit);
    bool committed(const HarnessCheckpoint& checkpoint, std::string_view event_type,
                   std::string* error = nullptr) override;

private:
    RunBindingResult advance(std::string_view binding_id, RunBindingState expected,
                             RunBindingState next, std::string_view diagnostic);
    void migrate();
    void* db_{nullptr};
    run::RunStore& runs_;
    HarnessStore& harnesses_;
    std::mutex mutex_;
};

}  // namespace agent_framework::harness
