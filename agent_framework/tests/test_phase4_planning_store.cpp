#include <cassert>
#include <filesystem>
#include <string>

#include "agent/internal/platform_io.hpp"
#include "agent/internal/sqlite_utils.hpp"
#include "agent/planning/plan_store.hpp"
#include <sqlite3.h>

namespace {
using namespace agent_framework;

contracts::ContractMetadata scope(std::string tenant = "tenant-a",
                                  std::string principal = "conversation-a") {
    contracts::ContractMetadata value;
    value.identity.tenant_id = std::move(tenant);
    value.identity.principal_id = std::move(principal);
    value.identity.project_id = "project-a";
    value.identity.task_id = "task-a";
    value.identity.plan_id = "plan-a";
    return value;
}

planning::ExecutionPlan plan() {
    planning::ExecutionPlan value;
    value.metadata = scope();
    value.task_understanding_digest = "sha256:understanding";
    value.evidence_bundle_digest = "sha256:evidence";
    value.acceptance_contract_digest = "sha256:acceptance";
    value.memory_snapshot_id = "sha256:snapshot";
    value.planning_view_digest = "sha256:view";
    value.nodes.push_back({"node-a", "durably implement", {"agent_framework"}, {}, {"input"},
                           {"output"}, {}, {"filesystem"}, {"write"}, "criterion-a",
                           "restore previous artifact", "medium", true});
    value.critical_path = {"node-a"};
    return value;
}
}  // namespace

int main() {
    using namespace agent_framework;
    const auto root = std::filesystem::temp_directory_path() /
        ("agent-phase4-planning-" + std::to_string(internal::current_process_id()));
    const auto path = root / "planning.sqlite3";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    std::string initial_digest;
    {
        planning::SQLitePlanningStore first(path.string());
        planning::EvidenceRecord evidence{"evidence-a", "repository", "repo://CMakeLists.txt",
            "sha256:evidence-a", "2026-08-09T00:00:00Z", "direct_observation", "",
            {"claim-a"}, {}, false};
        assert(first.append(scope(), evidence));
        assert(first.append(scope(), evidence).status == planning::PlanningCommitStatus::Duplicate);
        auto duplicate_content = evidence;
        duplicate_content.evidence_id = "evidence-b";
        assert(first.append(scope(), duplicate_content).status ==
               planning::PlanningCommitStatus::Duplicate);
        auto untrusted_instruction = evidence;
        untrusted_instruction.evidence_id = "evidence-external";
        untrusted_instruction.origin_kind = "external";
        untrusted_instruction.locator = "https://example.invalid/spec";
        untrusted_instruction.content_digest = "sha256:external";
        untrusted_instruction.instruction_authority = true;
        assert(first.append(scope(), untrusted_instruction).status ==
               planning::PlanningCommitStatus::Invalid);

        auto other_tenant_evidence = evidence;
        assert(first.append(scope("tenant-b"), other_tenant_evidence));
        assert(first.get(scope(), "evidence-a"));
        assert(first.get(scope("tenant-b"), "evidence-a"));
        assert(!first.get(scope("tenant-c"), "evidence-a"));
        auto other_conversation_evidence=evidence;
        other_conversation_evidence.locator="repo://other/CMakeLists.txt";
        other_conversation_evidence.content_digest="sha256:other-conversation";
        assert(first.append(scope("tenant-a","conversation-b"),other_conversation_evidence));
        assert(first.get(scope("tenant-a","conversation-b"),"evidence-a"));
        assert(first.get(scope(),"evidence-a")->content_digest=="sha256:evidence-a");
        const auto bundle = first.bundle(scope(), {"missing", "evidence-a"});
        assert(bundle.records.size() == 1 && !bundle.bundle_id.empty());

        const auto created = first.create(plan());
        assert(created);
        initial_digest = created.digest;
        assert(first.create(plan()).status == planning::PlanningCommitStatus::Duplicate);
        auto other_conversation_plan=plan();
        other_conversation_plan.metadata=scope("tenant-a","conversation-b");
        assert(first.create(other_conversation_plan));
        assert(first.current(other_conversation_plan.metadata.identity));

        planning::SQLitePlanningStore second(path.string());
        const auto recovered = second.current(scope().identity);
        assert(recovered && recovered->plan_revision == 1);
        auto revised = *recovered;
        revised.plan_revision = 2;
        revised.parent_plan_digest = initial_digest;
        revised.nodes.front().objective = "durably implement and verify";
        assert(second.compare_exchange(revised, 1));

        auto stale = revised;
        stale.plan_revision = 2;
        assert(first.compare_exchange(stale, 1).status ==
               planning::PlanningCommitStatus::RevisionConflict);
        assert(second.history(scope().identity).size() == 2);
    }

    {
        planning::SQLitePlanningStore recovered(path.string());
        const auto current = recovered.current(scope().identity);
        assert(current && current->plan_revision == 2 &&
               current->parent_plan_digest == initial_digest);
        assert(recovered.get(scope(), "evidence-a"));
    }

    // A v1 database is rebuilt into the conversation-scoped v2 keyspace.
    const auto migration_path=root/"planning-v1.sqlite3";
    sqlite3* legacy=nullptr;const auto legacy_open=sqlite3_open(migration_path.c_str(),&legacy);
    assert(legacy_open==SQLITE_OK);
    namespace sql=agent_framework::internal::sqlite;
    sql::exec(legacy,"CREATE TABLE planning_schema_version(version INTEGER PRIMARY KEY,applied_at TEXT NOT NULL)");
    sql::exec(legacy,"INSERT INTO planning_schema_version VALUES(1,'legacy')");
    sql::exec(legacy,"CREATE TABLE planning_evidence(tenant_id TEXT NOT NULL,task_id TEXT NOT NULL,evidence_id TEXT NOT NULL,locator TEXT NOT NULL,content_digest TEXT NOT NULL,document_json TEXT NOT NULL,created_at TEXT NOT NULL,PRIMARY KEY(tenant_id,task_id,evidence_id),UNIQUE(tenant_id,task_id,locator,content_digest))");
    sql::exec(legacy,"CREATE TABLE planning_plans(tenant_id TEXT NOT NULL,task_id TEXT NOT NULL,plan_id TEXT NOT NULL,revision INTEGER NOT NULL,plan_digest TEXT NOT NULL,parent_digest TEXT NOT NULL,document_json TEXT NOT NULL,created_at TEXT NOT NULL,PRIMARY KEY(tenant_id,task_id,plan_id,revision),UNIQUE(tenant_id,task_id,plan_id,plan_digest))");
    planning::EvidenceRecord legacy_record{"legacy-evidence","repository","repo://legacy",
        "sha256:legacy","legacy","direct_observation","",{"legacy-claim"},{},false};
    planning::EvidenceBundle legacy_bundle;legacy_bundle.metadata=scope();
    legacy_bundle.bundle_id="sha256:legacy";legacy_bundle.records={legacy_record};
    {
        sql::Statement insert(legacy,"INSERT INTO planning_evidence VALUES(?,?,?,?,?,?,?)");
        sql::bind_text(insert.get(),1,"tenant-a");sql::bind_text(insert.get(),2,"task-a");
        sql::bind_text(insert.get(),3,"legacy-evidence");sql::bind_text(insert.get(),4,"repo://legacy");
        sql::bind_text(insert.get(),5,"sha256:legacy");
        sql::bind_text(insert.get(),6,planning::encode(legacy_bundle).dump());
        sql::bind_text(insert.get(),7,"legacy");assert(sql::step(insert.get())==SQLITE_DONE);
    }
    const auto legacy_plan=plan();const auto legacy_plan_document=planning::encode(legacy_plan);
    {
        sql::Statement insert(legacy,"INSERT INTO planning_plans VALUES(?,?,?,?,?,?,?,?)");
        sql::bind_text(insert.get(),1,"tenant-a");sql::bind_text(insert.get(),2,"task-a");
        sql::bind_text(insert.get(),3,"plan-a");sql::bind_int64(insert.get(),4,1);
        sql::bind_text(insert.get(),5,legacy_plan_document.at("canonical_digest").get<std::string>());
        sql::bind_text(insert.get(),6,"");sql::bind_text(insert.get(),7,legacy_plan_document.dump());
        sql::bind_text(insert.get(),8,"legacy");assert(sql::step(insert.get())==SQLITE_DONE);
    }
    sqlite3_close(legacy);
    {
        planning::SQLitePlanningStore migrated(migration_path.string());
        assert(migrated.get(scope(),"legacy-evidence"));
        assert(migrated.current(scope().identity));
        assert(!migrated.get(scope("tenant-a","another-conversation"),"legacy-evidence"));
    }

#if !defined(_WIN32)
    const auto permissions = std::filesystem::status(path).permissions();
    assert((permissions & std::filesystem::perms::group_all) == std::filesystem::perms::none);
    assert((permissions & std::filesystem::perms::others_all) == std::filesystem::perms::none);
#endif
    std::filesystem::remove_all(root, error);
    return 0;
}
