#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>

#include "phase4_cognition_pipeline_test_support.hpp"

int main() {
    using namespace phase4_cognition_test;
    CognitionCheckpoint checkpoint;
    checkpoint.metadata = intake().metadata;
    checkpoint.pipeline_id = "pipeline-contract";
    checkpoint.intake_digest = planning::encode(intake()).at("canonical_digest");
    checkpoint.memory_snapshot_id = "snapshot-a";
    checkpoint.memory_view_digest = "sha256:view-a";
    checkpoint.updated_at = "2026-08-09T00:00:00Z";

    const auto document = planning::encode(checkpoint);
    std::vector<contracts::ContractIssue> issues;
    const auto decoded = planning::decode_cognition_checkpoint(document, {}, &issues);
    assert(decoded && issues.empty());
    assert(decoded->pipeline_id == checkpoint.pipeline_id);

    auto tampered = document;
    tampered["payload"]["state"] = "approved";
    issues.clear();
    assert(!planning::decode_cognition_checkpoint(tampered, {}, &issues));
    assert(!issues.empty());

    auto unknown = document;
    unknown["payload"]["unreviewed"] = true;
    unknown["canonical_digest"] = contracts::embedded_digest(
        json{{"schema_version", unknown.at("schema_version")},
             {"identity", unknown.at("identity")}, {"kind", unknown.at("kind")},
             {"payload", unknown.at("payload")}}).value();
    issues.clear();
    assert(!planning::decode_cognition_checkpoint(unknown, {}, &issues));

    InMemoryCognitionCheckpointStore memory;
    assert(memory.create(checkpoint));
    assert(memory.create(checkpoint).status == CognitionCheckpointStatus::AlreadyExists);
    auto next = checkpoint;
    next.revision = 2;
    next.next_stage = CognitionStage::Strategy;
    assert(memory.compare_exchange(next, 1));
    assert(memory.compare_exchange(next, 1).status ==
           CognitionCheckpointStatus::RevisionConflict);

    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto path = std::filesystem::temp_directory_path() /
                      ("taskflow-phase4-f2c-" + suffix + ".sqlite");
    {
        SQLiteCognitionCheckpointStore sqlite(path.string());
        assert(sqlite.create(checkpoint));
        auto durable = checkpoint;
        durable.revision = 2;
        durable.next_stage = CognitionStage::Investigation;
        assert(sqlite.compare_exchange(durable, 1));
        assert(sqlite.compare_exchange(durable, 1).status ==
               CognitionCheckpointStatus::RevisionConflict);
        assert(!sqlite.load("tenant-b", checkpoint.pipeline_id));
    }
    {
        SQLiteCognitionCheckpointStore reopened(path.string());
        const auto restored = reopened.load("tenant-a", checkpoint.pipeline_id);
        assert(restored && restored->revision == 2);
        assert(restored->checkpoint.next_stage == CognitionStage::Investigation);
    }
    const auto permissions = std::filesystem::status(path).permissions();
    assert((permissions & std::filesystem::perms::group_all) == std::filesystem::perms::none);
    assert((permissions & std::filesystem::perms::others_all) == std::filesystem::perms::none);
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + "-wal");
    std::filesystem::remove(path.string() + "-shm");
    return 0;
}
