#include <cassert>
#include <chrono>
#include <filesystem>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"
#include "phase4_memory_workflow_test_support.hpp"

int main() {
    using namespace phase4_memory_workflow_test;
    MemoryWorkflowCheckpoint checkpoint;
    checkpoint.metadata = metadata();
    checkpoint.workflow_id = "memory-contract";
    checkpoint.input_digest = "sha256:input";
    checkpoint.updated_at = "2026-08-10T00:00:00Z";

    const auto document = encode(checkpoint);
    std::vector<contracts::ContractIssue> issues;
    const auto decoded = decode_memory_workflow_checkpoint(document, {}, &issues);
    assert(decoded && issues.empty());
    assert(decoded->workflow_id == checkpoint.workflow_id);

    auto tampered = document;
    tampered["payload"]["state"] = "completed";
    issues.clear();
    assert(!decode_memory_workflow_checkpoint(tampered, {}, &issues));

    auto unknown = document;
    unknown["payload"]["unreviewed"] = true;
    unknown["canonical_digest"] = contracts::embedded_digest(
        json{{"schema_version", unknown.at("schema_version")},
             {"identity", unknown.at("identity")}, {"kind", unknown.at("kind")},
             {"payload", unknown.at("payload")}}).value();
    issues.clear();
    assert(!decode_memory_workflow_checkpoint(unknown, {}, &issues));

    InMemoryMemoryWorkflowCheckpointStore memory;
    assert(memory.create(checkpoint));
    assert(memory.create(checkpoint).status == MemoryWorkflowStoreStatus::AlreadyExists);
    auto next = checkpoint;
    next.revision = 2;
    next.next_stage = MemoryWorkflowStage::Normalization;
    assert(memory.compare_exchange(next, 1));
    assert(memory.compare_exchange(next, 1).status ==
           MemoryWorkflowStoreStatus::RevisionConflict);

    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto path = std::filesystem::temp_directory_path() /
                      ("taskflow-phase4-f3m-" + suffix + ".sqlite");
    {
        SQLiteMemoryWorkflowCheckpointStore sqlite(path.string());
        assert(sqlite.create(checkpoint));
        auto durable = checkpoint;
        durable.revision = 2;
        durable.next_stage = MemoryWorkflowStage::Consolidation;
        assert(sqlite.compare_exchange(durable, 1));
        assert(sqlite.compare_exchange(durable, 1).status ==
               MemoryWorkflowStoreStatus::RevisionConflict);
        assert(!sqlite.load("tenant-b", checkpoint.workflow_id));
    }
    {
        SQLiteMemoryWorkflowCheckpointStore reopened(path.string());
        const auto restored = reopened.load("tenant-a", checkpoint.workflow_id);
        assert(restored && restored->revision == 2);
        assert(restored->checkpoint.next_stage == MemoryWorkflowStage::Consolidation);
    }
    const auto permissions = std::filesystem::status(path).permissions();
    assert((permissions & std::filesystem::perms::group_all) == std::filesystem::perms::none);
    assert((permissions & std::filesystem::perms::others_all) == std::filesystem::perms::none);

    // Shared SQLite helpers preserve empty TEXT instead of silently binding SQL NULL.
    sqlite3* raw = nullptr;
    assert(sqlite3_open(":memory:", &raw) == SQLITE_OK);
    agent_framework::internal::sqlite::exec(raw, "CREATE TABLE values_table(value TEXT NOT NULL)");
    {
        agent_framework::internal::sqlite::Statement insert(
            raw, "INSERT INTO values_table(value) VALUES(?)");
        agent_framework::internal::sqlite::bind_text(insert.get(), 1, std::string_view{});
        assert(sqlite3_step(insert.get()) == SQLITE_DONE);
    }
    {
        agent_framework::internal::sqlite::Statement query(
            raw, "SELECT value, value IS NULL FROM values_table");
        assert(sqlite3_step(query.get()) == SQLITE_ROW);
        assert(agent_framework::internal::sqlite::column_text(query.get(), 0).empty());
        assert(sqlite3_column_int(query.get(), 1) == 0);
    }
    sqlite3_close(raw);

    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + "-wal");
    std::filesystem::remove(path.string() + "-shm");
    return 0;
}
