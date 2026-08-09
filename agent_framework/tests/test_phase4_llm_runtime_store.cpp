#include <cassert>
#include <filesystem>
#include <memory>
#include <string>

#include "agent/internal/platform_io.hpp"
#include "phase4_llm_runtime_test_support.hpp"

int main() {
    using namespace phase4_llm_test;
    const auto path = std::filesystem::temp_directory_path() /
        ("phase4-llm-runtime-" +
         std::to_string(agent_framework::internal::current_process_id()) + ".sqlite3");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto store = std::make_shared<SQLiteLLMRuntimeStore>(path.string());
        const auto p = profile();
        assert(store->publish_profile(p).status == RuntimeStoreStatus::Committed);
        assert(store->publish_profile(p).status == RuntimeStoreStatus::AlreadyExists);
        auto changed = p;
        changed.temperature = 0.2;
        assert(store->publish_profile(changed).status == RuntimeStoreStatus::RevisionConflict);
        assert(store->publish_prompt(prompt()).status == RuntimeStoreStatus::Committed);
        assert(store->publish_calibration(calibration()).status == RuntimeStoreStatus::Committed);
        assert(!store->load_profile("tenant-b", p.profile_id, p.revision));

        LLMInvocationManifest manifest;
        manifest.metadata = metadata();
        manifest.invocation_id = "recover-a";
        manifest.state = InvocationState::Pending;
        manifest.role = p.role;
        manifest.profile_id = p.profile_id;
        manifest.profile_revision = p.revision;
        manifest.prompt_id = p.prompt_id;
        manifest.prompt_revision = p.prompt_revision;
        manifest.prompt_digest = encode(prompt()).at("canonical_digest");
        auto created = store->create_invocation(manifest);
        assert(created.status == RuntimeStoreStatus::Committed && created.revision == 1);
        manifest.state = InvocationState::Running;
        auto running = store->update_invocation(manifest, created.revision);
        assert(running.status == RuntimeStoreStatus::Committed && running.revision == 2);
        assert(store->update_invocation(manifest, created.revision).status ==
               RuntimeStoreStatus::RevisionConflict);
        assert(store->list_recoverable("tenant-a", 10).size() == 1);
    }

    {
        auto store = std::make_shared<SQLiteLLMRuntimeStore>(path.string());
        assert(store->load_profile("tenant-a", "planning.architect", "profile-r1"));
        assert(store->load_prompt("tenant-a", "planning.prompt", "prompt-r1"));
        assert(store->load_calibration("tenant-a", "cal-r1"));
        auto client = std::make_shared<LLMClient>();
        auto router = std::make_shared<ModelRouter>();
        RoleRuntime runtime(client, store, router);
        const auto recovered = runtime.reconcile_recoverable("tenant-a", 10);
        assert(recovered.size() == 1);
        const auto loaded = store->load_invocation("tenant-a", "recover-a");
        assert(loaded && loaded->manifest.state == InvocationState::ManualReview);
        assert(loaded->manifest.error_code == "uncertain_provider_outcome_after_restart");
        assert(store->list_recoverable("tenant-a", 10).empty());
    }

    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
    return 0;
}
