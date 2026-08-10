#pragma once

#include <deque>
#include <map>

#include "agent/eval/judge_workflow.hpp"

namespace phase4_judge_test
{
    using namespace agent_framework;
    using namespace agent_framework::eval;
    using json = nlohmann::json;

    inline contracts::ContractMetadata metadata(std::string task = "task-f6e")
    {
        contracts::ContractMetadata m;
        m.identity.tenant_id = "tenant-a";
        m.identity.organization_id = "org-a";
        m.identity.principal_id = "user-a";
        m.identity.project_id = "project-a";
        m.identity.task_id = std::move(task);
        m.identity.run_id = m.identity.task_id + ":run";
        return m;
    }
    inline memory_v2::MemoryScope subject(const contracts::ContractMetadata &m)
    {
        memory_v2::MemoryScope s;
        s.tenant_id = m.identity.tenant_id;
        s.organization_id = m.identity.organization_id;
        s.principal_id = m.identity.principal_id;
        s.project_id = m.identity.project_id;
        s.task_id = m.identity.task_id;
        s.run_id = m.identity.run_id;
        s.level = memory_v2::MemoryLevel::Task;
        return s;
    }
    inline const std::vector<std::string> &metric_names()
    {
        static const std::vector<std::string> names = {
            "planning.evidence_citation", "planning.executability", "planning.dependency_completeness",
            "planning.acceptance_completeness", "memory.retrieval_utility", "memory.conflict_handling",
            "memory.freshness", "memory.promotion_precision", "memory.scope_leakage",
            "assurance.finding_precision", "assurance.finding_recall", "assurance.false_accept",
            "assurance.false_reject", "assurance.oracle_disagreement", "remediation.impact_precision",
            "remediation.reverify_precision", "remediation.loop_rate", "runtime.latency_ms", "runtime.tokens",
            "runtime.cost_usd", "runtime.retry_rate", "runtime.fallback_rate", "runtime.recovery_rate",
            "runtime.budget_compliance"};
        return names;
    }
    inline EvaluationSuite suite(std::string task = "task-f6e")
    {
        EvaluationSuite s;
        s.metadata = metadata(std::move(task));
        s.suite_id = "phase4-v2-f6e";
        s.dataset_version = "f6e-dataset-r1";
        s.seed = 42;
        s.cases = {{"case-1", DatasetLayer::Repository, 1.0, {"correctness", "evidence"}},
                   {"case-2", DatasetLayer::Repository, 1.0, {"correctness", "evidence"}}};
        for (const auto &name : metric_names())
        {
            MetricPolicy p;
            p.metric = name;
            p.layer = DatasetLayer::Repository;
            p.higher_is_better = name != "runtime.latency_ms" && name != "runtime.tokens" && name != "runtime.cost_usd" &&
                                 name != "runtime.retry_rate" && name != "runtime.fallback_rate" && name != "memory.scope_leakage" &&
                                 name != "assurance.false_accept" && name != "assurance.false_reject" &&
                                 name != "assurance.oracle_disagreement" && name != "remediation.loop_rate";
            p.maximum_regression = 0.0;
            p.critical_zero = name == "memory.scope_leakage" || name == "assurance.false_accept";
            if (name == "planning.executability" || name == "runtime.budget_compliance")
                p.minimum_candidate_value = 0.9;
            s.metric_policies.push_back(std::move(p));
        }
        s.minimum_judge_agreement = 0.7;
        s.minimum_ground_truth_accuracy = 0.7;
        s.maximum_flaky_delta = 0.1;
        return s;
    }
    inline void populate(DatasetRegistry &r, const EvaluationSuite &s)
    {
        for (const auto &sc : s.cases)
        {
            DatasetCase d;
            d.metadata = s.metadata;
            d.case_id = sc.case_id;
            d.input = "Evaluate " + sc.case_id;
            d.environment = {{"fixture", true}};
            d.ground_truth = {{"preferred_revision", "candidate"}};
            d.criterion_ids = sc.criterion_ids;
            d.tags = {dataset_layer_name(sc.layer)};
            d.license = "CC0-1.0";
            d.dataset_version = s.dataset_version;
            std::string error;
            if (!r.register_case(std::move(d), &error))
                throw std::runtime_error(error);
        }
    }
    inline CandidateEvaluationRun run(const EvaluationSuite &s, bool candidate)
    {
        CandidateEvaluationRun r;
        r.metadata = s.metadata;
        r.run_id = candidate ? "run-candidate" : "run-baseline";
        r.revision_id = candidate ? "candidate-r2" : "baseline-r1";
        r.profile_revision = candidate ? "profile-r2" : "profile-r1";
        r.prompt_revision = candidate ? "prompt-r2" : "prompt-r1";
        r.model_revision = candidate ? "model-r2" : "model-r1";
        r.executed = true;
        r.started_at = "2026-08-10T00:00:00Z";
        r.finished_at = "2026-08-10T00:01:00Z";
        for (const auto &sc : s.cases)
        {
            EvaluationCaseRun c;
            c.case_id = sc.case_id;
            c.trajectory.metadata = s.metadata;
            c.trajectory.case_id = sc.case_id;
            c.trajectory.component_version = candidate ? "component-r2" : "component-r1";
            c.trajectory.environment_digest = "sha256:environment";
            c.trajectory.event_digests = {"sha256:event"};
            c.trajectory.acceptance_report_digest = "sha256:acceptance";
            c.trajectory.started_at = r.started_at;
            c.trajectory.finished_at = r.finished_at;
            c.artifact = {{"answer", candidate ? "complete evidence-backed result" : "partial result"}, {"quality", candidate ? 0.95 : 0.65}};
            for (const auto &name : metric_names())
            {
                bool lower = name == "runtime.latency_ms" || name == "runtime.tokens" || name == "runtime.cost_usd" || name == "runtime.retry_rate" || name == "runtime.fallback_rate" || name == "memory.scope_leakage" || name == "assurance.false_accept" || name == "assurance.false_reject" || name == "assurance.oracle_disagreement" || name == "remediation.loop_rate";
                double value = lower ? (candidate ? 0.0 : 0.1) : (candidate ? 0.95 : 0.75);
                c.metrics[name] = value;
                c.metric_samples[name] = {value, value};
            }
            r.cases.push_back(std::move(c));
        }
        return r;
    }
    inline json verdicts_for(const JudgeStageRequest &r, bool prefer_best = true, bool unknown = false)
    {
        json verdicts = json::array();
        for (const auto &item : r.input.at("cases"))
        {
            auto a = item.at("artifacts").at("artifact-A").at("quality").get<double>();
            auto b = item.at("artifacts").at("artifact-B").at("quality").get<double>();
            std::string winner = prefer_best ? (a > b ? "artifact-A" : "artifact-B") : (a > b ? "artifact-B" : "artifact-A");
            if (unknown)
                winner = "candidate";
            std::map<std::string, double> criteria;
            for (const auto &id : item.at("criterion_ids").get<std::vector<std::string>>())
                criteria[id] = 0.9;
            verdicts.push_back({{"case_id", item.at("case_id")}, {"winner_alias", winner}, {"scores", {{"artifact-A", a}, {"artifact-B", b}}}, {"criterion_scores", criteria}, {"confidence", 0.9}, {"evidence_refs", {"artifact answer and quality"}}, {"risks", json::array()}});
        }
        return {{"verdicts", std::move(verdicts)}};
    }
    class ScriptedJudge final : public JudgeStageModel
    {
    public:
        bool secondary_disagrees{false};
        bool invalid_alias{false};
        int invocations{0};
        std::vector<JudgeStageRequest> requests;
        JudgeStageResponse invoke(const JudgeStageRequest &r) override
        {
            requests.push_back(r);
            invocations++;
            bool best = !(secondary_disagrees && r.stage == JudgeStage::SecondaryJudging);
            auto output = verdicts_for(r, best, invalid_alias);
            llm_runtime::LLMInvocationManifest m;
            m.metadata = r.metadata;
            m.invocation_id = r.workflow_id + ":" + judge_stage_name(r.stage) + ":" + std::to_string(r.attempt);
            m.state = llm_runtime::InvocationState::Succeeded;
            if (r.stage == JudgeStage::PrimaryJudging)
            {
                m.provider = "provider-a";
                m.model = "model-a";
                m.independence_group = "group-a";
            }
            else if (r.stage == JudgeStage::SecondaryJudging)
            {
                m.provider = "provider-b";
                m.model = "model-b";
                m.independence_group = "group-b";
            }
            else
            {
                m.provider = "provider-c";
                m.model = "model-c";
                m.independence_group = "group-c";
            }
            m.output_digest = contracts::embedded_digest(output).value();
            m.usage.input_tokens = 100;
            m.usage.output_tokens = 50;
            m.usage.cost_usd = 0.01;
            return {true, std::move(output), std::move(m), {}, {}};
        }
    };
    inline JudgeWorkflowOptions options(std::string id = "workflow-f6e")
    {
        JudgeWorkflowOptions o;
        o.workflow_id = std::move(id);
        o.max_stage_attempts = 2;
        o.require_upgrade_approval = false;
        o.approval_decision_id = "approval-f6e";
        o.approval_validator = [](std::string_view d, std::string_view id)
        { return d.rfind("sha256:", 0) == 0 && id == "approval-f6e"; };
        o.now = []
        { return "2026-08-10T00:02:00Z"; };
        return o;
    }
} // namespace phase4_judge_test
