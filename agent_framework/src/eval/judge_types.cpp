#include "agent/eval/judge_workflow.hpp"

#include <set>
#include <stdexcept>

namespace agent_framework::eval
{
    namespace
    {
        using json = nlohmann::json;

        void fields(const json &value, const std::set<std::string> &names, std::string_view path)
        {
            if (!contracts::validate_object_fields(value, names, names,
                                                   contracts::UnknownFieldPolicy::Reject, nullptr, nullptr, path))
                throw std::invalid_argument("invalid or unknown fields at " + std::string(path));
        }

        json suite_case_json(const SuiteCase &v)
        {
            return {{"case_id", v.case_id}, {"layer", dataset_layer_name(v.layer)}, {"weight", v.weight}, {"criterion_ids", v.criterion_ids}};
        }
        SuiteCase suite_case_value(const json &v)
        {
            fields(v, {"case_id", "layer", "weight", "criterion_ids"}, "/suite_case");
            auto layer = dataset_layer_from_name(v.at("layer").get<std::string>());
            if (!layer)
                throw std::invalid_argument("unknown dataset layer");
            return {v.at("case_id").get<std::string>(), *layer, v.at("weight").get<double>(),
                    v.at("criterion_ids").get<std::vector<std::string>>()};
        }
        json metric_policy_json(const MetricPolicy &v)
        {
            return {{"metric", v.metric}, {"layer", dataset_layer_name(v.layer)}, {"higher_is_better", v.higher_is_better}, {"maximum_regression", v.maximum_regression}, {"minimum_candidate_value", v.minimum_candidate_value ? json(*v.minimum_candidate_value) : json(nullptr)}, {"critical_zero", v.critical_zero}};
        }
        MetricPolicy metric_policy_value(const json &v)
        {
            fields(v, {"metric", "layer", "higher_is_better", "maximum_regression", "minimum_candidate_value", "critical_zero"}, "/metric_policy");
            auto layer = dataset_layer_from_name(v.at("layer").get<std::string>());
            if (!layer)
                throw std::invalid_argument("unknown metric dataset layer");
            MetricPolicy out;
            out.metric = v.at("metric").get<std::string>();
            out.layer = *layer;
            out.higher_is_better = v.at("higher_is_better").get<bool>();
            out.maximum_regression = v.at("maximum_regression").get<double>();
            if (!v.at("minimum_candidate_value").is_null())
                out.minimum_candidate_value = v.at("minimum_candidate_value").get<double>();
            out.critical_zero = v.at("critical_zero").get<bool>();
            return out;
        }
        json case_run_json(const EvaluationCaseRun &v)
        {
            return {{"case_id", v.case_id}, {"trajectory", encode(v.trajectory)}, {"artifact", v.artifact}, {"metrics", v.metrics}, {"metric_samples", v.metric_samples}, {"error", v.error}};
        }
        EvaluationCaseRun case_run_value(const json &v)
        {
            fields(v, {"case_id", "trajectory", "artifact", "metrics", "metric_samples", "error"},
                   "/evaluation_case_run");
            auto trajectory = decode_trajectory(v.at("trajectory"));
            if (!trajectory)
                throw std::invalid_argument("invalid trajectory contract");
            return {v.at("case_id").get<std::string>(), std::move(*trajectory), v.at("artifact"),
                    v.at("metrics").get<std::map<std::string, double>>(),
                    v.at("metric_samples").get<std::map<std::string, std::vector<double>>>(),
                    v.at("error").get<std::string>()};
        }
        json assignment_json(const BlindAssignment &v)
        {
            return {{"case_id", v.case_id}, {"baseline_alias", v.baseline_alias}, {"candidate_alias", v.candidate_alias}, {"primary_order", v.primary_order}, {"secondary_order", v.secondary_order}, {"baseline_artifact_digest", v.baseline_artifact_digest}, {"candidate_artifact_digest", v.candidate_artifact_digest}};
        }
        BlindAssignment assignment_value(const json &v)
        {
            fields(v, {"case_id", "baseline_alias", "candidate_alias", "primary_order", "secondary_order", "baseline_artifact_digest", "candidate_artifact_digest"},
                   "/blind_assignment");
            return {v.at("case_id").get<std::string>(), v.at("baseline_alias").get<std::string>(),
                    v.at("candidate_alias").get<std::string>(),
                    v.at("primary_order").get<std::vector<std::string>>(),
                    v.at("secondary_order").get<std::vector<std::string>>(),
                    v.at("baseline_artifact_digest").get<std::string>(),
                    v.at("candidate_artifact_digest").get<std::string>()};
        }
        json verdict_json(const JudgeVerdict &v)
        {
            return {{"case_id", v.case_id}, {"winner_alias", v.winner_alias}, {"scores", v.scores}, {"criterion_scores", v.criterion_scores}, {"confidence", v.confidence}, {"evidence_refs", v.evidence_refs}, {"risks", v.risks}};
        }
        JudgeVerdict verdict_value(const json &v)
        {
            fields(v, {"case_id", "winner_alias", "scores", "criterion_scores", "confidence", "evidence_refs", "risks"}, "/judge_verdict");
            return {v.at("case_id").get<std::string>(), v.at("winner_alias").get<std::string>(),
                    v.at("scores").get<std::map<std::string, double>>(),
                    v.at("criterion_scores").get<std::map<std::string, double>>(),
                    v.at("confidence").get<double>(), v.at("evidence_refs").get<std::vector<std::string>>(),
                    v.at("risks").get<std::vector<std::string>>()};
        }
        json batch_payload(const JudgeBatch &v)
        {
            json verdicts = json::array();
            for (const auto &item : v.verdicts)
                verdicts.push_back(verdict_json(item));
            return {{"batch_id", v.batch_id}, {"judge_role", v.judge_role}, {"invocation_id", v.invocation_id}, {"independence_group", v.independence_group}, {"provider", v.provider}, {"model", v.model}, {"verdicts", std::move(verdicts)}};
        }
        JudgeBatch batch_value(const json &v)
        {
            fields(v, {"batch_id", "judge_role", "invocation_id", "independence_group", "provider", "model", "verdicts"}, "/judge_batch");
            JudgeBatch out;
            out.batch_id = v.at("batch_id").get<std::string>();
            out.judge_role = v.at("judge_role").get<std::string>();
            out.invocation_id = v.at("invocation_id").get<std::string>();
            out.independence_group = v.at("independence_group").get<std::string>();
            out.provider = v.at("provider").get<std::string>();
            out.model = v.at("model").get<std::string>();
            for (const auto &item : v.at("verdicts"))
                out.verdicts.push_back(verdict_value(item));
            return out;
        }
        json calibration_payload(const JudgeCalibrationReport &v)
        {
            return {{"calibration_id", v.calibration_id}, {"raw_agreement", v.raw_agreement}, {"cohens_kappa", v.cohens_kappa}, {"first_position_win_rate", v.first_position_win_rate}, {"score_delta_variance", v.score_delta_variance}, {"ground_truth_accuracy", v.ground_truth_accuracy}, {"ground_truth_samples", v.ground_truth_samples}, {"candidate_win_rate", v.candidate_win_rate}, {"candidate_win_ci95_low", v.candidate_win_ci95_low}, {"candidate_win_ci95_high", v.candidate_win_ci95_high}, {"disputed_case_ids", v.disputed_case_ids}, {"calibration_findings", v.calibration_findings}};
        }
        JudgeCalibrationReport calibration_value(const json &v)
        {
            fields(v, {"calibration_id", "raw_agreement", "cohens_kappa", "first_position_win_rate", "score_delta_variance", "ground_truth_accuracy", "ground_truth_samples", "candidate_win_rate", "candidate_win_ci95_low", "candidate_win_ci95_high", "disputed_case_ids", "calibration_findings"},
                   "/calibration");
            JudgeCalibrationReport out;
            out.calibration_id = v.at("calibration_id").get<std::string>();
            out.raw_agreement = v.at("raw_agreement").get<double>();
            out.cohens_kappa = v.at("cohens_kappa").get<double>();
            out.first_position_win_rate = v.at("first_position_win_rate").get<double>();
            out.score_delta_variance = v.at("score_delta_variance").get<double>();
            out.ground_truth_accuracy = v.at("ground_truth_accuracy").get<double>();
            out.ground_truth_samples = v.at("ground_truth_samples").get<std::uint64_t>();
            out.candidate_win_rate = v.at("candidate_win_rate").get<double>();
            out.candidate_win_ci95_low = v.at("candidate_win_ci95_low").get<double>();
            out.candidate_win_ci95_high = v.at("candidate_win_ci95_high").get<double>();
            out.disputed_case_ids = v.at("disputed_case_ids").get<std::vector<std::string>>();
            out.calibration_findings = v.at("calibration_findings").get<std::vector<std::string>>();
            return out;
        }
        json comparison_json(const MetricComparison &v)
        {
            return {{"metric", v.metric}, {"baseline_mean", v.baseline_mean}, {"candidate_mean", v.candidate_mean}, {"paired_delta", v.paired_delta}, {"ci95_half_width", v.ci95_half_width}, {"regression", v.regression}};
        }
        MetricComparison comparison_value(const json &v)
        {
            fields(v, {"metric", "baseline_mean", "candidate_mean", "paired_delta", "ci95_half_width", "regression"},
                   "/metric_comparison");
            return {v.at("metric").get<std::string>(), v.at("baseline_mean").get<double>(),
                    v.at("candidate_mean").get<double>(), v.at("paired_delta").get<double>(),
                    v.at("ci95_half_width").get<double>(), v.at("regression").get<bool>()};
        }
        json metrics_payload(const QualityMetricReport &v)
        {
            json comparisons = json::array();
            for (const auto &item : v.comparisons)
                comparisons.push_back(comparison_json(item));
            return {{"report_id", v.report_id}, {"comparisons", std::move(comparisons)}, {"missing_metrics", v.missing_metrics}, {"flaky_metrics", v.flaky_metrics}, {"critical_violations", v.critical_violations}, {"candidate_means", v.candidate_means}};
        }
        QualityMetricReport metrics_value(const json &v)
        {
            fields(v, {"report_id", "comparisons", "missing_metrics", "flaky_metrics", "critical_violations", "candidate_means"}, "/quality_metrics");
            QualityMetricReport out;
            out.report_id = v.at("report_id").get<std::string>();
            for (const auto &item : v.at("comparisons"))
                out.comparisons.push_back(comparison_value(item));
            out.missing_metrics = v.at("missing_metrics").get<std::vector<std::string>>();
            out.flaky_metrics = v.at("flaky_metrics").get<std::vector<std::string>>();
            out.critical_violations = v.at("critical_violations").get<std::vector<std::string>>();
            out.candidate_means = v.at("candidate_means").get<std::map<std::string, double>>();
            return out;
        }
        json decision_payload(const UpgradeDecision &v)
        {
            return {{"decision_id", v.decision_id}, {"outcome", upgrade_outcome_name(v.outcome)}, {"baseline_revision", v.baseline_revision}, {"candidate_revision", v.candidate_revision}, {"rollback_revision", v.rollback_revision}, {"calibration_digest", v.calibration_digest}, {"metric_report_digest", v.metric_report_digest}, {"reasons", v.reasons}, {"approval_request_digest", v.approval_request_digest}, {"approval_decision_id", v.approval_decision_id}};
        }
        UpgradeDecision decision_value(const json &v)
        {
            fields(v, {"decision_id", "outcome", "baseline_revision", "candidate_revision", "rollback_revision", "calibration_digest", "metric_report_digest", "reasons", "approval_request_digest", "approval_decision_id"}, "/upgrade_decision");
            auto outcome = upgrade_outcome_from_name(v.at("outcome").get<std::string>());
            if (!outcome)
                throw std::invalid_argument("unknown upgrade outcome");
            return {{}, v.at("decision_id").get<std::string>(), *outcome, v.at("baseline_revision").get<std::string>(), v.at("candidate_revision").get<std::string>(), v.at("rollback_revision").get<std::string>(), v.at("calibration_digest").get<std::string>(), v.at("metric_report_digest").get<std::string>(), v.at("reasons").get<std::vector<std::string>>(), v.at("approval_request_digest").get<std::string>(), v.at("approval_decision_id").get<std::string>()};
        }
        json artifact_json(const JudgeStageArtifact &v)
        {
            return {{"stage", judge_stage_name(v.stage)}, {"attempt", v.attempt}, {"invocation_id", v.invocation_id}, {"output_digest", v.output_digest}, {"provider", v.provider}, {"model", v.model}, {"independence_group", v.independence_group}, {"tokens", v.tokens}, {"cost_usd", v.cost_usd}, {"output", v.output}};
        }
        JudgeStageArtifact artifact_value(const json &v)
        {
            fields(v, {"stage", "attempt", "invocation_id", "output_digest", "provider", "model", "independence_group", "tokens", "cost_usd", "output"}, "/judge_stage_artifact");
            auto stage = judge_stage_from_name(v.at("stage").get<std::string>());
            if (!stage)
                throw std::invalid_argument("unknown judge stage");
            return {*stage, v.at("attempt").get<std::uint64_t>(), v.at("invocation_id").get<std::string>(),
                    v.at("output_digest").get<std::string>(), v.at("provider").get<std::string>(),
                    v.at("model").get<std::string>(), v.at("independence_group").get<std::string>(),
                    v.at("tokens").get<std::uint64_t>(), v.at("cost_usd").get<double>(), v.at("output")};
        }

        template <class T, class Builder>
        std::optional<T> document(const json &value, const char *kind,
                                  const std::set<std::string> &names, const contracts::ParseContext &context,
                                  std::vector<contracts::ContractIssue> *issues, Builder builder)
        {
            auto parsed = contracts::parse_typed_contract(value, kind, context, issues);
            if (!parsed || !contracts::validate_object_fields(parsed->payload, names, names,
                                                              contracts::UnknownFieldPolicy::Reject, nullptr, issues, "/payload"))
                return std::nullopt;
            try
            {
                auto out = builder(parsed->payload);
                out.metadata = std::move(parsed->metadata);
                return out;
            }
            catch (const std::exception &e)
            {
                contracts::append_issue(issues, "payload_decode_failed", "/payload", e.what());
                return std::nullopt;
            }
        }
    } // namespace

    std::string dataset_layer_name(DatasetLayer v)
    {
        switch (v)
        {
        case DatasetLayer::Unit:
            return "unit";
        case DatasetLayer::Repository:
            return "repository";
        case DatasetLayer::Domain:
            return "domain";
        case DatasetLayer::Adversarial:
            return "adversarial";
        case DatasetLayer::Recovery:
            return "recovery";
        case DatasetLayer::Live:
            return "live";
        }
        return "unit";
    }
    std::optional<DatasetLayer> dataset_layer_from_name(std::string_view v)
    {
        if (v == "unit")
            return DatasetLayer::Unit;
        if (v == "repository")
            return DatasetLayer::Repository;
        if (v == "domain")
            return DatasetLayer::Domain;
        if (v == "adversarial")
            return DatasetLayer::Adversarial;
        if (v == "recovery")
            return DatasetLayer::Recovery;
        if (v == "live")
            return DatasetLayer::Live;
        return std::nullopt;
    }
    std::string judge_stage_name(JudgeStage v)
    {
        switch (v)
        {
        case JudgeStage::Preparation:
            return "preparation";
        case JudgeStage::PrimaryJudging:
            return "primary_judging";
        case JudgeStage::SecondaryJudging:
            return "secondary_judging";
        case JudgeStage::Calibration:
            return "calibration";
        case JudgeStage::Adjudication:
            return "adjudication";
        case JudgeStage::Metrics:
            return "metrics";
        case JudgeStage::UpgradeGate:
            return "upgrade_gate";
        case JudgeStage::Complete:
            return "complete";
        }
        return "preparation";
    }
    std::optional<JudgeStage> judge_stage_from_name(std::string_view v)
    {
        if (v == "preparation")
            return JudgeStage::Preparation;
        if (v == "primary_judging")
            return JudgeStage::PrimaryJudging;
        if (v == "secondary_judging")
            return JudgeStage::SecondaryJudging;
        if (v == "calibration")
            return JudgeStage::Calibration;
        if (v == "adjudication")
            return JudgeStage::Adjudication;
        if (v == "metrics")
            return JudgeStage::Metrics;
        if (v == "upgrade_gate")
            return JudgeStage::UpgradeGate;
        if (v == "complete")
            return JudgeStage::Complete;
        return std::nullopt;
    }
    std::string judge_workflow_state_name(JudgeWorkflowState v)
    {
        switch (v)
        {
        case JudgeWorkflowState::Running:
            return "running";
        case JudgeWorkflowState::AwaitingApproval:
            return "awaiting_approval";
        case JudgeWorkflowState::Approved:
            return "approved";
        case JudgeWorkflowState::Rejected:
            return "rejected";
        case JudgeWorkflowState::ManualReview:
            return "manual_review";
        case JudgeWorkflowState::Failed:
            return "failed";
        case JudgeWorkflowState::Cancelled:
            return "cancelled";
        }
        return "failed";
    }
    std::optional<JudgeWorkflowState> judge_workflow_state_from_name(std::string_view v)
    {
        if (v == "running")
            return JudgeWorkflowState::Running;
        if (v == "awaiting_approval")
            return JudgeWorkflowState::AwaitingApproval;
        if (v == "approved")
            return JudgeWorkflowState::Approved;
        if (v == "rejected")
            return JudgeWorkflowState::Rejected;
        if (v == "manual_review")
            return JudgeWorkflowState::ManualReview;
        if (v == "failed")
            return JudgeWorkflowState::Failed;
        if (v == "cancelled")
            return JudgeWorkflowState::Cancelled;
        return std::nullopt;
    }
    std::string upgrade_outcome_name(UpgradeOutcome v)
    {
        switch (v)
        {
        case UpgradeOutcome::AwaitingApproval:
            return "awaiting_approval";
        case UpgradeOutcome::Approved:
            return "approved";
        case UpgradeOutcome::Rejected:
            return "rejected";
        case UpgradeOutcome::ManualReview:
            return "manual_review";
        }
        return "manual_review";
    }
    std::optional<UpgradeOutcome> upgrade_outcome_from_name(std::string_view v)
    {
        if (v == "awaiting_approval")
            return UpgradeOutcome::AwaitingApproval;
        if (v == "approved")
            return UpgradeOutcome::Approved;
        if (v == "rejected")
            return UpgradeOutcome::Rejected;
        if (v == "manual_review")
            return UpgradeOutcome::ManualReview;
        return std::nullopt;
    }

    json encode(const EvaluationSuite &v)
    {
        json cases = json::array(), policies = json::array();
        for (const auto &i : v.cases)
            cases.push_back(suite_case_json(i));
        for (const auto &i : v.metric_policies)
            policies.push_back(metric_policy_json(i));
        return contracts::make_typed_contract(v.metadata, "agent.evaluation_suite/v1", {{"suite_id", v.suite_id}, {"revision", v.revision}, {"dataset_version", v.dataset_version}, {"seed", v.seed}, {"cases", std::move(cases)}, {"metric_policies", std::move(policies)}, {"minimum_judge_agreement", v.minimum_judge_agreement}, {"minimum_ground_truth_accuracy", v.minimum_ground_truth_accuracy}, {"maximum_flaky_delta", v.maximum_flaky_delta}, {"requires_live_execution", v.requires_live_execution}});
    }
    json encode(const CandidateEvaluationRun &v)
    {
        json cases = json::array();
        for (const auto &i : v.cases)
            cases.push_back(case_run_json(i));
        return contracts::make_typed_contract(v.metadata, "agent.candidate_evaluation_run/v1", {{"run_id", v.run_id}, {"revision_id", v.revision_id}, {"profile_revision", v.profile_revision}, {"prompt_revision", v.prompt_revision}, {"model_revision", v.model_revision}, {"executed", v.executed}, {"cases", std::move(cases)}, {"started_at", v.started_at}, {"finished_at", v.finished_at}});
    }
    json encode(const JudgeBatch &v) { return contracts::make_typed_contract(v.metadata, "agent.judge_batch/v1", batch_payload(v)); }
    json encode(const JudgeCalibrationReport &v) { return contracts::make_typed_contract(v.metadata, "agent.judge_calibration/v1", calibration_payload(v)); }
    json encode(const QualityMetricReport &v) { return contracts::make_typed_contract(v.metadata, "agent.quality_metric_report/v1", metrics_payload(v)); }
    json encode(const UpgradeDecision &v) { return contracts::make_typed_contract(v.metadata, "agent.upgrade_decision/v1", decision_payload(v)); }
    json encode(const EvaluationReport &v) { return contracts::make_typed_contract(v.metadata, "agent.evaluation_report/v1", {{"workflow_id", v.workflow_id}, {"run_kind", v.run_kind}, {"executed", v.executed}, {"suite_digest", v.suite_digest}, {"baseline_run_digest", v.baseline_run_digest}, {"candidate_run_digest", v.candidate_run_digest}, {"primary", batch_payload(v.primary)}, {"secondary", batch_payload(v.secondary)}, {"adjudication", v.adjudication ? batch_payload(*v.adjudication) : json(nullptr)}, {"calibration", calibration_payload(v.calibration)}, {"metrics", metrics_payload(v.metrics)}, {"decision", decision_payload(v.decision)}, {"trend_key", v.trend_key}, {"created_at", v.created_at}}); }
    json encode(const JudgeCheckpoint &v)
    {
        json assignments = json::array(), artifacts = json::array();
        for (const auto &i : v.assignments)
            assignments.push_back(assignment_json(i));
        for (const auto &i : v.artifacts)
            artifacts.push_back(artifact_json(i));
        return contracts::make_typed_contract(v.metadata, "agent.judge_checkpoint/v1", {{"workflow_id", v.workflow_id}, {"revision", v.revision}, {"state", judge_workflow_state_name(v.state)}, {"next_stage", judge_stage_name(v.next_stage)}, {"stage_attempts", v.stage_attempts}, {"completed_stages", v.completed_stages}, {"suite_digest", v.suite_digest}, {"baseline_run_digest", v.baseline_run_digest}, {"candidate_run_digest", v.candidate_run_digest}, {"memory_snapshot_id", v.memory_snapshot_id}, {"memory_view_digest", v.memory_view_digest}, {"assignments", std::move(assignments)}, {"primary", v.primary ? batch_payload(*v.primary) : json(nullptr)}, {"secondary", v.secondary ? batch_payload(*v.secondary) : json(nullptr)}, {"adjudication", v.adjudication ? batch_payload(*v.adjudication) : json(nullptr)}, {"calibration", v.calibration ? calibration_payload(*v.calibration) : json(nullptr)}, {"metrics", v.metrics ? metrics_payload(*v.metrics) : json(nullptr)}, {"decision", v.decision ? decision_payload(*v.decision) : json(nullptr)}, {"artifacts", std::move(artifacts)}, {"consumed_tokens", v.consumed_tokens}, {"consumed_cost_usd", v.consumed_cost_usd}, {"evaluation_report_digest", v.evaluation_report_digest}, {"error_code", v.error_code}, {"error_message", v.error_message}, {"updated_at", v.updated_at}});
    }

    std::optional<EvaluationSuite> decode_evaluation_suite(const json &v, const contracts::ParseContext &c, std::vector<contracts::ContractIssue> *i)
    {
        static const std::set<std::string> f = {"suite_id", "revision", "dataset_version", "seed", "cases", "metric_policies", "minimum_judge_agreement", "minimum_ground_truth_accuracy", "maximum_flaky_delta", "requires_live_execution"};
        return document<EvaluationSuite>(v, "agent.evaluation_suite/v1", f, c, i, [](const json &p)
                                         {EvaluationSuite o;o.suite_id=p.at("suite_id").get<std::string>();o.revision=p.at("revision").get<std::uint64_t>();o.dataset_version=p.at("dataset_version").get<std::string>();o.seed=p.at("seed").get<std::uint64_t>();for(const auto&x:p.at("cases"))o.cases.push_back(suite_case_value(x));for(const auto&x:p.at("metric_policies"))o.metric_policies.push_back(metric_policy_value(x));o.minimum_judge_agreement=p.at("minimum_judge_agreement").get<double>();o.minimum_ground_truth_accuracy=p.at("minimum_ground_truth_accuracy").get<double>();o.maximum_flaky_delta=p.at("maximum_flaky_delta").get<double>();o.requires_live_execution=p.at("requires_live_execution").get<bool>();return o; });
    }
    std::optional<CandidateEvaluationRun> decode_candidate_evaluation_run(const json &v, const contracts::ParseContext &c, std::vector<contracts::ContractIssue> *i)
    {
        static const std::set<std::string> f = {"run_id", "revision_id", "profile_revision", "prompt_revision", "model_revision", "executed", "cases", "started_at", "finished_at"};
        return document<CandidateEvaluationRun>(v, "agent.candidate_evaluation_run/v1", f, c, i, [](const json &p)
                                                {CandidateEvaluationRun o;o.run_id=p.at("run_id").get<std::string>();o.revision_id=p.at("revision_id").get<std::string>();o.profile_revision=p.at("profile_revision").get<std::string>();o.prompt_revision=p.at("prompt_revision").get<std::string>();o.model_revision=p.at("model_revision").get<std::string>();o.executed=p.at("executed").get<bool>();for(const auto&x:p.at("cases"))o.cases.push_back(case_run_value(x));o.started_at=p.at("started_at").get<std::string>();o.finished_at=p.at("finished_at").get<std::string>();return o; });
    }
    std::optional<EvaluationReport> decode_evaluation_report(const json &v, const contracts::ParseContext &c, std::vector<contracts::ContractIssue> *i)
    {
        static const std::set<std::string> f = {"workflow_id", "run_kind", "executed", "suite_digest", "baseline_run_digest", "candidate_run_digest", "primary", "secondary", "adjudication", "calibration", "metrics", "decision", "trend_key", "created_at"};
        return document<EvaluationReport>(v, "agent.evaluation_report/v1", f, c, i, [](const json &p)
                                          {EvaluationReport o;o.workflow_id=p.at("workflow_id").get<std::string>();o.run_kind=p.at("run_kind").get<std::string>();o.executed=p.at("executed").get<bool>();o.suite_digest=p.at("suite_digest").get<std::string>();o.baseline_run_digest=p.at("baseline_run_digest").get<std::string>();o.candidate_run_digest=p.at("candidate_run_digest").get<std::string>();o.primary=batch_value(p.at("primary"));o.secondary=batch_value(p.at("secondary"));if(!p.at("adjudication").is_null())o.adjudication=batch_value(p.at("adjudication"));o.calibration=calibration_value(p.at("calibration"));o.metrics=metrics_value(p.at("metrics"));o.decision=decision_value(p.at("decision"));o.trend_key=p.at("trend_key").get<std::string>();o.created_at=p.at("created_at").get<std::string>();return o; });
    }
    std::optional<JudgeCheckpoint> decode_judge_checkpoint(const json &v, const contracts::ParseContext &c, std::vector<contracts::ContractIssue> *i)
    {
        static const std::set<std::string> f = {"workflow_id", "revision", "state", "next_stage", "stage_attempts", "completed_stages", "suite_digest", "baseline_run_digest", "candidate_run_digest", "memory_snapshot_id", "memory_view_digest", "assignments", "primary", "secondary", "adjudication", "calibration", "metrics", "decision", "artifacts", "consumed_tokens", "consumed_cost_usd", "evaluation_report_digest", "error_code", "error_message", "updated_at"};
        return document<JudgeCheckpoint>(v, "agent.judge_checkpoint/v1", f, c, i, [](const json &p)
                                         {JudgeCheckpoint o;o.workflow_id=p.at("workflow_id").get<std::string>();o.revision=p.at("revision").get<std::uint64_t>();auto state=judge_workflow_state_from_name(p.at("state").get<std::string>());auto stage=judge_stage_from_name(p.at("next_stage").get<std::string>());if(!state||!stage)throw std::invalid_argument("unknown judge state/stage");o.state=*state;o.next_stage=*stage;o.stage_attempts=p.at("stage_attempts").get<std::map<std::string,std::uint64_t>>();o.completed_stages=p.at("completed_stages").get<std::vector<std::string>>();o.suite_digest=p.at("suite_digest").get<std::string>();o.baseline_run_digest=p.at("baseline_run_digest").get<std::string>();o.candidate_run_digest=p.at("candidate_run_digest").get<std::string>();o.memory_snapshot_id=p.at("memory_snapshot_id").get<std::string>();o.memory_view_digest=p.at("memory_view_digest").get<std::string>();for(const auto&x:p.at("assignments"))o.assignments.push_back(assignment_value(x));if(!p.at("primary").is_null())o.primary=batch_value(p.at("primary"));if(!p.at("secondary").is_null())o.secondary=batch_value(p.at("secondary"));if(!p.at("adjudication").is_null())o.adjudication=batch_value(p.at("adjudication"));if(!p.at("calibration").is_null())o.calibration=calibration_value(p.at("calibration"));if(!p.at("metrics").is_null())o.metrics=metrics_value(p.at("metrics"));if(!p.at("decision").is_null())o.decision=decision_value(p.at("decision"));for(const auto&x:p.at("artifacts"))o.artifacts.push_back(artifact_value(x));o.consumed_tokens=p.at("consumed_tokens").get<std::uint64_t>();o.consumed_cost_usd=p.at("consumed_cost_usd").get<double>();o.evaluation_report_digest=p.at("evaluation_report_digest").get<std::string>();o.error_code=p.at("error_code").get<std::string>();o.error_message=p.at("error_message").get<std::string>();o.updated_at=p.at("updated_at").get<std::string>();return o; });
    }

} // namespace agent_framework::eval
