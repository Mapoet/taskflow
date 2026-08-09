#include <cassert>
#include <chrono>
#include <filesystem>
#include <memory>

#include "agent/memory_v2/workflows/legacy_memory_provider.hpp"
#include "phase4_memory_workflow_test_support.hpp"

namespace {
using namespace phase4_memory_workflow_test;

struct ResultWithRequests {
    MemoryWorkflowResult result;
    std::vector<MemoryStageRequest> requests;
};

class LeakyProvider final : public MemoryProvider {
public:
    std::string id() const override { return "leaky"; }
    ProviderResult fetch(const MemoryQuery&) override {
        auto record = existing_record(metadata());
        record.scope.tenant_id = "tenant-b";
        record.metadata.identity.tenant_id = "tenant-b";
        return {id(), 1, {std::move(record)}, {}};
    }
};

ResultWithRequests run_case(const MemoryWorkflowInput& value, ScriptedMemoryModel& model,
                            const std::filesystem::path& path) {
    auto store = std::make_shared<SQLiteMemoryStore>(path.string());
    assert(store->append(existing_record(value.metadata)));
    MemoryProviderRegistry providers;
    assert(providers.register_provider(std::make_shared<StoreMemoryProvider>("store", store)));
    MemoryViewEngine views(providers);
    InMemoryMemoryWorkflowCheckpointStore checkpoints;
    MultiLayerMemoryWorkflow workflow(views, store, checkpoints, model);
    auto result = workflow.run(value);
    return {std::move(result), model.requests};
}
}

int main() {
    using namespace phase4_memory_workflow_test;
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() /
                      ("taskflow-phase4-f3m-negative-" + suffix);
    std::filesystem::create_directories(root);

    // An LLM cannot recover or cite a source that scope-first filtering excluded.
    {
        auto value = input("workflow-negative-source");
        ScriptedMemoryModel model;
        model.push(MemoryWorkflowStage::Extraction, extraction_output(true));
        const auto outcome = run_case(value, model, root / "source.sqlite");
        assert(outcome.result.state == MemoryWorkflowState::Failed);
        assert(outcome.result.error_code == "memory_extraction_failed");
        assert(outcome.result.error_message.find("invisible source") != std::string::npos);
    }

    // Rerank output is confined to already scope/ACL-filtered records.
    {
        auto value = input("workflow-negative-rerank");
        ScriptedMemoryModel model;
        model.push(MemoryWorkflowStage::Extraction, extraction_output());
        model.push(MemoryWorkflowStage::Normalization, normalization_output());
        model.push(MemoryWorkflowStage::Consolidation, consolidation_output(value));
        model.push(MemoryWorkflowStage::ConflictResolution, conflict_output());
        model.push(MemoryWorkflowStage::TaskStateUpdate, task_state_output());
        model.push(MemoryWorkflowStage::QueryPlanning, query_plan_output(),
                   "query-provider", "query-model", "query");
        model.push(MemoryWorkflowStage::Reranking, reranking_output(value, true),
                   "rerank-provider", "rerank-model", "rerank");
        const auto outcome = run_case(value, model, root / "rerank.sqlite");
        assert(outcome.result.state == MemoryWorkflowState::Failed);
        assert(outcome.result.error_code == "memory_reranking_failed");
        assert(outcome.result.error_message.find("unknown or duplicate") != std::string::npos);
    }

    // An LLM cannot invent a workflow-phase transition outside the deterministic router.
    {
        auto value = input("workflow-negative-view");
        ScriptedMemoryModel model;
        model.push(MemoryWorkflowStage::Extraction, extraction_output());
        model.push(MemoryWorkflowStage::Normalization, normalization_output());
        model.push(MemoryWorkflowStage::Consolidation, consolidation_output(value));
        model.push(MemoryWorkflowStage::ConflictResolution, conflict_output());
        model.push(MemoryWorkflowStage::TaskStateUpdate, task_state_output());
        model.push(MemoryWorkflowStage::QueryPlanning, query_plan_output(),
                   "query-provider", "query-model", "query");
        model.push(MemoryWorkflowStage::Reranking, reranking_output(value),
                   "rerank-provider", "rerank-model", "rerank");
        model.push(MemoryWorkflowStage::DynamicView, dynamic_view_output(value, "execution"));
        const auto outcome = run_case(value, model, root / "view.sqlite");
        assert(outcome.result.state == MemoryWorkflowState::Failed);
        assert(outcome.result.error_code == "memory_dynamic_view_transition_denied");
    }

    // v1 dual-read projects legacy content as non-authoritative data and enforces fixed scope.
    {
        auto legacy = std::make_shared<agent_framework::MemoryStore>(
            std::make_unique<agent_framework::InMemoryBackend>());
        agent_framework::Message message;
        message.role = "user";
        message.content = "legacy conversation value";
        message.timestamp = 1;
        legacy->store_message("legacy-session", message);
        agent_framework::MemorySummary summary;
        summary.session_id = "legacy-session";
        summary.summary = "legacy summary value";
        summary.keywords = {"legacy"};
        summary.created_at = 1;
        summary.updated_at = 1;
        legacy->store_long_term_memory("legacy-session", summary);

        memory_v2::workflows::LegacyMemoryProviderConfig config;
        config.fixed_scope = input().subject;
        config.fixed_scope.level = MemoryLevel::Task;
        config.session_id = "legacy-session";
        config.summary_query = "legacy";
        memory_v2::workflows::LegacyMemoryProvider provider(legacy, config);
        MemoryQuery query;
        query.subject = config.fixed_scope;
        query.principal_id = config.fixed_scope.principal_id;
        const auto visible = provider.fetch(query);
        assert(visible.error.empty() && visible.records.size() == 2);
        for(const auto& record : visible.records) {
            assert(record.authority == Authority::Observed);
            assert(record.status == MemoryStatus::Candidate);
            assert(record.source_kind == "legacy_memory_v1");
            assert(!record.content.value("instruction_authority", true));
        }
        query.subject.tenant_id = "tenant-b";
        assert(provider.fetch(query).records.empty());
    }

    // ViewEngine distrusts provider-side filtering and fails closed on a leaked record.
    {
        MemoryProviderRegistry providers;
        assert(providers.register_provider(std::make_shared<LeakyProvider>()));
        MemoryViewEngine views(providers);
        const auto value = input("workflow-leaky-provider");
        auto spec = make_view_spec(MemoryViewMode::Planning, value.metadata, value.subject);
        const auto view = views.build(spec);
        assert(view.fail_closed);
        assert(view.error == "provider_scope_violation:leaky");
    }

    std::error_code error;
    std::filesystem::remove_all(root, error);
    return 0;
}
