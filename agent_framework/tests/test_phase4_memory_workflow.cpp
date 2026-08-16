#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <filesystem>
#include <memory>

#include "phase4_memory_workflow_test_support.hpp"

int main() {
    using namespace phase4_memory_workflow_test;
    const auto value = input();
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto path = std::filesystem::temp_directory_path() /
                      ("taskflow-phase4-f3m-memory-" + suffix + ".sqlite");
    auto store = std::make_shared<SQLiteMemoryStore>(path.string());
    assert(store->append(existing_record(value.metadata)));
    MemoryProviderRegistry providers;
    assert(providers.register_provider(std::make_shared<StoreMemoryProvider>("store", store)));
    MemoryViewEngine views(providers);
    InMemoryMemoryWorkflowCheckpointStore checkpoints;
    ScriptedMemoryModel model;
    script_success(model, value);
    MultiLayerMemoryWorkflow workflow(views, store, checkpoints, model);

    std::vector<MemoryWorkflowEvent> events;
    MemoryWorkflowOptions options;
    options.now = [] { return "2026-08-10T00:00:00Z"; };
    options.event_sink = [&](const MemoryWorkflowEvent& event) { events.push_back(event); };
    auto first = workflow.run(value, options);
    assert(first.state == MemoryWorkflowState::AwaitingApproval);
    assert(first.candidates.size() == 2);
    assert(first.recommendations.size() == 1);
    assert(first.checkpoint.reranked_record_ids.front() == candidate_id(value));
    assert(!first.base_view.fail_closed && !first.dynamic_view.fail_closed);

    // Scope/ACL filtering happens before every LLM stage. Tool text remains data, not instruction.
    assert(!model.requests.empty());
    const auto& visible_sources = model.requests.front().input.at("sources");
    assert(visible_sources.size() == 2);
    bool tool_was_data = false;
    for(const auto& source : visible_sources) {
        assert(source.at("artifact_id") != "source-secret");
        if(source.at("artifact_id") == "source-tool")
            tool_was_data = !source.at("instruction_authority").get<bool>();
    }
    assert(tool_was_data);
    assert(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.event_type == "source_excluded";
    }));

    auto candidate = store->current(candidate_id(value));
    assert(candidate && candidate->status == MemoryStatus::Candidate &&
           candidate->authority == Authority::Candidate);
    const auto request_count = model.requests.size();

    // Approval closes the recommendation workflow but does not mutate authority by itself.
    options.approval_decision_id = "decision-f3m";
    options.governance_validator = [](const json& recommendation, std::string_view decision) {
        return decision == "decision-f3m" &&
               recommendation.at("recommendation_id") == "promote-compat";
    };
    auto approved = workflow.run(value, options);
    assert(approved.state == MemoryWorkflowState::Completed);
    assert(model.requests.size() == request_count);
    candidate = store->current(candidate_id(value));
    assert(candidate && candidate->status == MemoryStatus::Candidate);

    MemoryGovernanceService governance(store);
    MemoryRecommendationExecutor executor(store, governance);
    auto denied = executor.execute(first.recommendations.front(), "", "", {});
    assert(denied.commit.status == CommitStatus::Forbidden);
    auto promoted = executor.execute(
        first.recommendations.front(), "decision-f3m", "",
        [](const json&, std::string_view decision) { return decision == "decision-f3m"; });
    assert(promoted.commit);
    candidate = store->current(candidate_id(value));
    assert(candidate && candidate->status == MemoryStatus::Verified &&
           candidate->authority == Authority::Verified);

    auto cross_scope = first.recommendations.front();
    cross_scope["record_id"] = task_state_id(value);
    cross_scope["target_level"] = "project";
    auto cross_scope_denied = executor.execute(
        cross_scope, "decision-f3m", "approval-f3m",
        [](const json&, std::string_view) { return true; });
    assert(cross_scope_denied.commit.status == CommitStatus::Forbidden);

    auto forget = first.recommendations.front();
    forget["recommendation_id"] = "forget-task-state";
    forget["action"] = "forget";
    forget["record_id"] = task_state_id(value);
    forget["target_level"] = "task";
    auto forget_without_approval = executor.execute(
        forget, "decision-forget", "",
        [](const json&, std::string_view) { return true; });
    assert(forget_without_approval.commit.status == CommitStatus::Forbidden);
    auto forgotten = executor.execute(
        forget, "decision-forget", "approval-forget",
        [](const json&, std::string_view) { return true; });
    assert(forgotten.commit);
    assert(store->current(task_state_id(value))->status == MemoryStatus::Tombstoned);

    std::error_code error;
    store.reset();
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
    return 0;
}
