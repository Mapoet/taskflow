#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>

#include "agent/internal/platform_io.hpp"
#include "agent/memory_v2/governance.hpp"
#include "agent/memory_v2/view_engine.hpp"

namespace {
using namespace agent_framework;

memory_v2::MemoryRecord record(std::string id, std::string tenant, std::string project,
                               memory_v2::MemoryLevel level = memory_v2::MemoryLevel::Project) {
    memory_v2::MemoryRecord value;
    value.metadata.identity.tenant_id = tenant;
    value.metadata.identity.project_id = project;
    value.metadata.identity.memory_id = id;
    value.record_id = std::move(id);
    value.scope.tenant_id = std::move(tenant);
    value.scope.project_id = std::move(project);
    value.scope.level = level;
    value.kind = memory_v2::MemoryKind::Semantic;
    value.authority = memory_v2::Authority::Candidate;
    value.status = memory_v2::MemoryStatus::Candidate;
    value.source_kind = "test";
    value.content_type = "application/json";
    value.content = {{"fact", value.record_id}};
    return value;
}

memory_v2::MemoryViewSpec spec() {
    memory_v2::MemoryViewSpec value;
    value.metadata.identity.tenant_id = "tenant-a";
    value.metadata.identity.principal_id = "user-a";
    value.metadata.identity.project_id = "project-a";
    value.metadata.identity.task_id = "task-a";
    value.metadata.extensions["policy_revision"] = "policy-v1";
    value.workflow_phase = "planning";
    value.subject.tenant_id = "tenant-a";
    value.subject.organization_id = "org-a";
    value.subject.principal_id = "user-a";
    value.subject.project_id = "project-a";
    value.subject.task_id = "task-a";
    value.allowed_levels = {memory_v2::MemoryLevel::System, memory_v2::MemoryLevel::Project};
    value.authority_floor = memory_v2::Authority::Derived;
    value.byte_budget = 1024 * 1024;
    return value;
}

class RecordingForgetSink final : public memory_v2::ForgetSink {
public:
    std::string id() const override { return "derived-index"; }
    bool erase(std::string_view record_id, std::string*) override {
        erased = std::string(record_id);
        return true;
    }
    std::string erased;
};
}

int main() {
    using namespace agent_framework;
    const auto root = std::filesystem::temp_directory_path() /
        ("agent-phase4-memory-" + std::to_string(internal::current_process_id()));
    const auto path = root / "memory.sqlite3";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    std::string first_view_digest;
    {
        auto store = std::make_shared<memory_v2::SQLiteMemoryStore>(path.string());
        auto project = record("project-rule", "tenant-a", "project-a");
        const auto initial_commit = store->append(project);
        if(!initial_commit) std::cerr << "initial memory commit failed: " << initial_commit.error << '\n';
        assert(initial_commit);
        project.revision = 2;
        memory_v2::MemoryGovernanceService governance(store);
        assert(governance.promote("project-rule", 1, memory_v2::MemoryStatus::Verified,
                                  memory_v2::Authority::Verified, "verification-1"));
        const auto stale_revision = store->revise(project, 1);
        assert(stale_revision.status == memory_v2::CommitStatus::Invalid ||
               stale_revision.status == memory_v2::CommitStatus::RevisionConflict);

        auto system = record("system-rule", "tenant-a", "", memory_v2::MemoryLevel::System);
        system.scope.project_id.clear();
        system.kind = memory_v2::MemoryKind::Instruction;
        system.authority = memory_v2::Authority::Authoritative;
        system.status = memory_v2::MemoryStatus::Authoritative;
        assert(store->append(system).status == memory_v2::CommitStatus::Invalid);
        assert(store->append(system, "approval-system"));

        auto other_project = record("other-project", "tenant-a", "project-b");
        assert(store->append(other_project));
        auto other_tenant = record("other-tenant", "tenant-b", "project-a");
        assert(store->append(other_tenant));
        auto private_record = record("private", "tenant-a", "project-a");
        private_record.acl_principals = {"different-user"};
        assert(store->append(private_record));

        auto forget_me = record("forget-me", "tenant-a", "project-a");
        assert(store->append(forget_me));
        auto sink = std::make_shared<RecordingForgetSink>();
        assert(governance.register_forget_sink(sink));
        const auto forgotten = governance.forget("forget-me", 1, "approval-forget");
        assert(forgotten.commit);
        assert(forgotten.completed_sinks == std::vector<std::string>{"derived-index"});
        assert(forgotten.inconclusive_sinks.empty());
        assert(sink->erased == "forget-me");
        assert(!store->current("forget-me")->source_locator.size());

        memory_v2::MemoryQuery query;
        query.subject = spec().subject;
        query.principal_id = "user-a";
        auto visible = store->query(query);
        assert(visible.size() == 2);
        assert(store->generation() == 8);
        assert(store->history("project-rule").size() == 2);

        memory_v2::MemoryProviderRegistry providers;
        assert(providers.register_provider(
            std::make_shared<memory_v2::StoreMemoryProvider>("local", store)));
        memory_v2::MemoryViewEngine engine(providers);
        auto view_spec = spec();
        view_spec.mandatory_record_ids = {"system-rule"};
        auto view = engine.build(view_spec, "2026-08-09T00:00:00Z");
        assert(!view.fail_closed);
        assert(view.records.size() == 2);
        assert(view.manifest.selected.front().record_id == "system-rule");
        assert(!view.manifest.view_digest.empty());
        first_view_digest = view.manifest.view_digest;
        auto repeated = engine.build(view_spec, "2026-08-09T00:00:00Z");
        assert(repeated.manifest.view_digest == first_view_digest);

        view_spec.byte_budget = 1;
        auto too_small = engine.build(view_spec, "2026-08-09T00:00:00Z");
        assert(too_small.fail_closed);
    }

    {
        auto store = std::make_shared<memory_v2::SQLiteMemoryStore>(path.string());
        memory_v2::MemoryProviderRegistry providers;
        assert(providers.register_provider(
            std::make_shared<memory_v2::StoreMemoryProvider>("local", store)));
        memory_v2::MemoryViewEngine engine(providers);
        auto view_spec = spec();
        view_spec.mandatory_record_ids = {"system-rule"};
        auto recovered = engine.build(view_spec, "2026-08-09T00:00:00Z");
        assert(recovered.manifest.view_digest == first_view_digest);
    }
    std::filesystem::remove_all(root, error);
    return 0;
}
