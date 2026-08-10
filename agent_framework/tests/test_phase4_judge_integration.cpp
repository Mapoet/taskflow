#include <cassert>
#include <chrono>
#include <filesystem>

#include <taskflow/taskflow.hpp>

#include "agent/eval/judge_graph_template.hpp"
#include "agent/internal/agent_thread_state.hpp"
#include "agent/prompt_renderer/prompt_renderer.hpp"
#include "phase4_judge_test_support.hpp"
#include "phase4_llm_runtime_test_support.hpp"

namespace
{
    using namespace phase4_judge_test;
    std::string profile_id(JudgeStage stage) { return "f6e." + judge_stage_name(stage); }
    json static_verdicts(const EvaluationSuite &s)
    {
        json verdicts = json::array();
        for (const auto &sc : s.cases)
        {
            bool flip = (contracts::embedded_digest(json{{"seed", s.seed}, {"suite", s.suite_id}, {"case", sc.case_id}}).value().back() % 2) != 0;
            std::string candidate = flip ? "artifact-A" : "artifact-B";
            std::map<std::string, double> criteria;
            for (const auto &id : sc.criterion_ids)
                criteria[id] = 0.9;
            verdicts.push_back({{"case_id", sc.case_id}, {"winner_alias", candidate}, {"scores", {{"artifact-A", candidate == "artifact-A" ? 0.95 : 0.65}, {"artifact-B", candidate == "artifact-B" ? 0.95 : 0.65}}}, {"criterion_scores", criteria}, {"confidence", 0.9}, {"evidence_refs", {"anonymous artifact evidence"}}, {"risks", json::array()}});
        }
        return {{"verdicts", std::move(verdicts)}};
    }
    void publish_role(const std::shared_ptr<llm_runtime::LLMRuntimeStore> &store, const contracts::ContractMetadata &m, JudgeStage stage, std::string candidate, std::string provider, std::string model)
    {
        llm_runtime::LLMRoleProfile p;
        p.metadata = m;
        p.profile_id = profile_id(stage);
        p.revision = "r1";
        p.role = p.profile_id;
        p.provider_pool = {candidate};
        p.reasoning_effort = llm_runtime::ReasoningEffort::High;
        p.max_context_tokens = 16384;
        p.max_output_tokens = 4096;
        p.prompt_id = p.profile_id + ".prompt";
        p.prompt_revision = "r1";
        p.memory_view_profile = "evaluation";
        p.allowed_regions = {"local"};
        p.independence_group = p.profile_id + ".group";
        p.evidence_authority = llm_runtime::EvidenceAuthority::Advisory;
        p.timeout_ms = 5000;
        p.max_attempts = 1;
        p.max_fallbacks = 0;
        p.calibration_revision = p.profile_id + ".cal";
        llm_runtime::PromptRevision prompt;
        prompt.metadata = m;
        prompt.prompt_id = p.prompt_id;
        prompt.revision = "r1";
        prompt.system_template = "Blindly judge anonymous artifacts.";
        prompt.user_template = "{{input}}";
        prompt.input_schema = {{"type", "object"}, {"properties", {{"input", {{"type", "string"}}}}}, {"required", {"input"}}, {"additionalProperties", false}};
        prompt.output_schema = {{"type", "object"}, {"additionalProperties", true}};
        prompt.structured_output_required = true;
        prompt.max_repair_attempts = 0;
        prompt.compatibility_class = "f6e-r1";
        llm_runtime::RoleCalibrationRecord cal;
        cal.metadata = m;
        cal.calibration_id = p.calibration_revision;
        cal.role = p.role;
        cal.profile_id = p.profile_id;
        cal.profile_revision = p.revision;
        cal.prompt_revision = "r1";
        cal.provider = provider;
        cal.model = model;
        cal.dataset_revision = "f6e-offline-r1";
        cal.metrics = {{"schema_success", 1.0}};
        cal.thresholds = {{"schema_success", 0.99}};
        cal.approved = true;
        cal.decision_id = "offline-fixture";
        assert(store->publish_profile(p).ok() && store->publish_prompt(prompt).ok() && store->publish_calibration(cal).ok());
    }
}

int main()
{
    using namespace phase4_judge_test;
    {
        auto rs = suite("task-f6e-role-runtime");
        DatasetRegistry rd;
        populate(rd, rs);
        auto rb = run(rs, false), rc = run(rs, true);
        auto runtime_store = std::make_shared<llm_runtime::InMemoryLLMRuntimeStore>();
        auto router = std::make_shared<llm_runtime::ModelRouter>();
        auto client = std::make_shared<LLMClient>();
        client->set_prompt_renderer(std::make_shared<PromptRenderer>());
        std::vector<std::shared_ptr<phase4_llm_test::ScriptedAdapter>> adapters;
        for (std::size_t n = 0; n < 2; ++n)
        {
            auto stage = n == 0 ? JudgeStage::PrimaryJudging : JudgeStage::SecondaryJudging;
            auto label = std::to_string(n + 1);
            auto candidate = "f6e-candidate-" + label, provider = "fake-f6e-" + label, model_name = "f6e-model-" + label;
            auto route = phase4_llm_test::candidate(candidate, provider, model_name, 10);
            route.independence_group = "f6e-group-" + label;
            assert(router->register_candidate(route));
            publish_role(runtime_store, rs.metadata, stage, candidate, provider, model_name);
            auto adapter = std::make_shared<phase4_llm_test::ScriptedAdapter>();
            auto output = static_verdicts(rs);
            adapter->push([output]
                          { return phase4_llm_test::output(output.dump()); });
            client->register_adapter(provider, adapter);
            adapters.push_back(adapter);
        }
        auto runtime = std::make_shared<llm_runtime::RoleRuntime>(client, runtime_store, router, nullptr, nullptr, llm_runtime::RoleRuntimeOptions{true, true, []
                                                                                                                                                   { return "2026-08-10T00:00:00Z"; },
                                                                                                                                                   {}});
        RoleRuntimeJudgeModel stage_model(runtime);
        assert(stage_model.bind(JudgeStage::PrimaryJudging, {profile_id(JudgeStage::PrimaryJudging), "r1", {}, "local"}));
        assert(stage_model.bind(JudgeStage::SecondaryJudging, {profile_id(JudgeStage::SecondaryJudging), "r1", {}, "local"}));
        memory_v2::MemoryProviderRegistry rp;
        memory_v2::MemoryViewEngine rv(rp);
        InMemoryJudgeStore store;
        LLMJudgeWorkflow workflow(rv, store, stage_model);
        auto ro = options("role-runtime-f6e");
        auto result = workflow.run(rs, rd, rb, rc, subject(rs.metadata), ro);
        assert(result.state == JudgeWorkflowState::Approved);
        for (const auto &a : adapters)
            assert(a->calls() == 1);
        for (const auto stage : {JudgeStage::PrimaryJudging, JudgeStage::SecondaryJudging})
        {
            auto invocation = runtime_store->load_invocation("tenant-a", ro.workflow_id + ":" + judge_stage_name(stage) + ":1");
            assert(invocation && invocation->manifest.memory_view_profile == "evaluation" && invocation->manifest.profile_id == profile_id(stage));
        }
    }
    auto s = suite("task-f6e-integration");
    auto data = std::make_shared<DatasetRegistry>();
    populate(*data, s);
    auto b = run(s, false), c = run(s, true);
    memory_v2::MemoryProviderRegistry providers;
    memory_v2::MemoryViewEngine views(providers);
    auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto path = (std::filesystem::temp_directory_path() / ("taskflow-f6e-integration-" + suffix + ".sqlite")).string();
    ScriptedJudge model;
    {
        SQLiteJudgeStore store(path);
        LLMJudgeWorkflow workflow(views, store, model);
        auto first = options("restart-f6e");
        first.require_upgrade_approval = true;
        first.approval_decision_id.clear();
        first.approval_validator = {};
        auto waiting = workflow.run(s, *data, b, c, subject(s.metadata), first);
        assert(waiting.state == JudgeWorkflowState::AwaitingApproval && !waiting.report);
        assert(model.requests.size() == 2);
    }
    {
        SQLiteJudgeStore store(path);
        auto workflow = std::make_shared<LLMJudgeWorkflow>(views, store, model);
        auto resumed = options("restart-f6e");
        resumed.require_upgrade_approval = true;
        resumed.approval_decision_id = "approval-f6e";
        resumed.approval_validator = [](std::string_view d, std::string_view id)
        { return d.rfind("sha256:", 0) == 0 && id == "approval-f6e"; };
        auto approved = workflow->run(s, *data, b, c, subject(s.metadata), resumed);
        assert(approved.state == JudgeWorkflowState::Approved && approved.report && model.requests.size() == 2);
        auto report = store.load_report("tenant-a", "restart-f6e");
        assert(report && report->report.decision.approval_decision_id == "approval-f6e");
        auto gs = suite("task-f6e-graph");
        auto gd = std::make_shared<DatasetRegistry>();
        populate(*gd, gs);
        auto gb = run(gs, false), gc = run(gs, true);
        auto graph_options = options("graph-f6e");
        auto graph_workflow = std::make_shared<LLMJudgeWorkflow>(views, store, model);
        auto templ = std::make_shared<JudgeGraphTemplate>(graph_workflow, gs, gd, gb, gc, subject(gs.metadata), graph_options);
        GraphExecutor graph;
        graph.register_template(templ->get_template_name(), templ);
        tf::Executor executor;
        ExecutionRequest request;
        request.template_id = templ->get_template_name();
        request.session = std::make_shared<internal::AgentThreadState>();
        request.session->initial_user_prompt = "Evaluate candidate revision";
        request.context.task_id = gs.metadata.identity.task_id;
        request.context.tenant_id = gs.metadata.identity.tenant_id;
        request.context.session_id = "session-f6e";
        request.options.input_already_processed = true;
        request.options.persist_session = false;
        auto executed = graph.execute_sync(executor, request);
        assert(executed.success && executed.outputs.at("state") == "approved" && executed.outputs.contains("evaluation_report"));
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
    return 0;
}
