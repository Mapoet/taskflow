#include "agent/eval/judge_workflow.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>

namespace agent_framework::eval
{
    namespace
    {
        using json = nlohmann::json;
        std::string now_utc()
        {
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            std::tm utc{};
#ifdef _WIN32
            gmtime_s(&utc, &t);
#else
            gmtime_r(&t, &utc);
#endif
            std::ostringstream s;
            s << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
            return s.str();
        }
        std::string digest(const json &v)
        {
            auto d = contracts::embedded_digest(v);
            if (!d)
                throw std::runtime_error("canonical digest failed");
            return *d;
        }
        bool has(const std::vector<std::string> &v, std::string_view x) { return std::find(v.begin(), v.end(), x) != v.end(); }
        bool semantic(JudgeStage s) { return s == JudgeStage::PrimaryJudging || s == JudgeStage::SecondaryJudging || s == JudgeStage::Adjudication; }
        bool final_state(JudgeWorkflowState s) { return s == JudgeWorkflowState::Approved || s == JudgeWorkflowState::Rejected || s == JudgeWorkflowState::ManualReview; }
        std::uint64_t tokens(const llm_runtime::LLMInvocationManifest &m) { return m.usage.input_tokens.value_or(0) + m.usage.output_tokens.value_or(0); }
        double cost(const llm_runtime::LLMInvocationManifest &m) { return m.usage.cost_usd.value_or(0.0); }
        json memory_context(const memory_v2::MemoryView &v)
        {
            json records = json::array();
            for (const auto &r : v.records)
                records.push_back(memory_v2::encode(r));
            return {{"snapshot_id", v.snapshot.snapshot_id}, {"view_digest", v.manifest.view_digest}, {"records", std::move(records)}, {"instruction_authority", false}};
        }
        bool finite(double v) { return std::isfinite(v); }
        bool protected_artifact(const json &v)
        {
            static const std::set<std::string> blocked = {
                "ground_truth", "preferred_revision", "provider_identity", "model_identity",
                "profile_id", "profile_revision", "prompt_revision", "model_revision",
                "revision_id", "candidate_id", "baseline_id", "evaluation_label"};
            if (v.is_object())
                for (const auto &[k, x] : v.items())
                {
                    if (blocked.count(k) || protected_artifact(x))
                        return true;
                }
            else if (v.is_array())
                for (const auto &x : v)
                    if (protected_artifact(x))
                        return true;
            return false;
        }

        bool validate_inputs(const EvaluationSuite &s, const DatasetRegistry &datasets, const CandidateEvaluationRun &b,
                             const CandidateEvaluationRun &c, const memory_v2::MemoryScope &subject, std::string *error)
        {
            if (s.metadata.identity.tenant_id.empty() || s.metadata.identity.task_id.empty() || s.suite_id.empty() ||
                s.revision == 0 || s.dataset_version.empty() || s.cases.empty() || s.metric_policies.empty() ||
                !finite(s.minimum_judge_agreement) || s.minimum_judge_agreement < 0 || s.minimum_judge_agreement > 1 ||
                !finite(s.minimum_ground_truth_accuracy) || s.minimum_ground_truth_accuracy < 0 || s.minimum_ground_truth_accuracy > 1 ||
                !finite(s.maximum_flaky_delta) || s.maximum_flaky_delta < 0 || !b.executed || !c.executed ||
                b.revision_id.empty() || c.revision_id.empty() || b.revision_id == c.revision_id ||
                b.metadata.identity.tenant_id != s.metadata.identity.tenant_id || c.metadata.identity.tenant_id != s.metadata.identity.tenant_id ||
                b.metadata.identity.task_id != s.metadata.identity.task_id || c.metadata.identity.task_id != s.metadata.identity.task_id ||
                subject.tenant_id != s.metadata.identity.tenant_id || subject.task_id != s.metadata.identity.task_id)
            {
                if (error)
                    *error = "suite, executed runs, identities, revisions or thresholds are invalid";
                return false;
            }
            std::map<std::string, const EvaluationCaseRun *> bm, cm;
            for (const auto &x : b.cases)
                if (x.case_id.empty() || !bm.emplace(x.case_id, &x).second)
                {
                    if (error)
                        *error = "baseline case ids must be non-empty and unique";
                    return false;
                }
            for (const auto &x : c.cases)
                if (x.case_id.empty() || !cm.emplace(x.case_id, &x).second)
                {
                    if (error)
                        *error = "candidate case ids must be non-empty and unique";
                    return false;
                }
            std::set<std::string> ids, metrics;
            for (const auto &p : s.metric_policies)
            {
                if (p.metric.empty() || !metrics.insert(p.metric).second || !finite(p.maximum_regression) || p.maximum_regression < 0 || (p.minimum_candidate_value && !finite(*p.minimum_candidate_value)))
                {
                    if (error)
                        *error = "metric policies must be finite, named and unique";
                    return false;
                }
            }
            for (const auto &sc : s.cases)
            {
                auto dc = datasets.case_for_scoring(sc.case_id);
                if (sc.case_id.empty() || !ids.insert(sc.case_id).second || !dc || dc->dataset_version != s.dataset_version || dc->metadata.identity.tenant_id != s.metadata.identity.tenant_id || !finite(sc.weight) || sc.weight <= 0 || sc.criterion_ids.empty() || sc.criterion_ids != dc->criterion_ids || !bm.count(sc.case_id) || !cm.count(sc.case_id))
                {
                    if (error)
                        *error = "suite cases must uniquely bind dataset version, criteria and both runs";
                    return false;
                }
                const auto &br = *bm.at(sc.case_id);
                const auto &cr = *cm.at(sc.case_id);
                if (br.trajectory.case_id != sc.case_id || cr.trajectory.case_id != sc.case_id || br.trajectory.environment_digest.empty() || cr.trajectory.environment_digest.empty() || !br.error.empty() || !cr.error.empty() || !br.artifact.is_object() || !cr.artifact.is_object() || protected_artifact(br.artifact) || protected_artifact(cr.artifact) || protected_artifact(dc->environment))
                {
                    if (error)
                        *error = "case trajectories/artifacts are invalid, failed, or expose protected blind fields";
                    return false;
                }
                for (const auto &[_, v] : br.metrics)
                    if (!finite(v))
                    {
                        if (error)
                            *error = "baseline metric is non-finite";
                        return false;
                    }
                for (const auto &[_, v] : cr.metrics)
                    if (!finite(v))
                    {
                        if (error)
                            *error = "candidate metric is non-finite";
                        return false;
                    }
                for (const auto &[_, vs] : cr.metric_samples)
                    for (double v : vs)
                        if (!finite(v))
                        {
                            if (error)
                                *error = "candidate metric sample is non-finite";
                            return false;
                        }
                if ((s.requires_live_execution || sc.layer == DatasetLayer::Live) && (b.started_at.empty() || b.finished_at.empty() || c.started_at.empty() || c.finished_at.empty()))
                {
                    if (error)
                        *error = "live evaluation requires timestamped executed runs";
                    return false;
                }
            }
            return bm.size() == s.cases.size() && cm.size() == s.cases.size();
        }
        std::map<std::string, const EvaluationCaseRun *> case_map(const CandidateEvaluationRun &r)
        {
            std::map<std::string, const EvaluationCaseRun *> m;
            for (const auto &x : r.cases)
                m[x.case_id] = &x;
            return m;
        }
        std::vector<BlindAssignment> assignments(const EvaluationSuite &s, const CandidateEvaluationRun &b, const CandidateEvaluationRun &c)
        {
            auto bm = case_map(b), cm = case_map(c);
            std::vector<BlindAssignment> out;
            for (const auto &sc : s.cases)
            {
                const bool flip = (digest(json{{"seed", s.seed}, {"suite", s.suite_id}, {"case", sc.case_id}}).back() % 2) != 0;
                BlindAssignment a;
                a.case_id = sc.case_id;
                a.baseline_alias = flip ? "artifact-B" : "artifact-A";
                a.candidate_alias = flip ? "artifact-A" : "artifact-B";
                a.primary_order = {"artifact-A", "artifact-B"};
                a.secondary_order = {"artifact-B", "artifact-A"};
                a.baseline_artifact_digest = digest(bm[sc.case_id]->artifact);
                a.candidate_artifact_digest = digest(cm[sc.case_id]->artifact);
                out.push_back(std::move(a));
            }
            return out;
        }
        const BlindAssignment *assignment_for(const std::vector<BlindAssignment> &v, std::string_view id)
        {
            auto it = std::find_if(v.begin(), v.end(), [&](const auto &x)
                                   { return x.case_id == id; });
            return it == v.end() ? nullptr : &*it;
        }
        std::string semantic_winner(const JudgeVerdict &v, const BlindAssignment &a)
        {
            if (v.winner_alias == "tie")
                return "tie";
            if (v.winner_alias == a.candidate_alias)
                return "candidate";
            if (v.winner_alias == a.baseline_alias)
                return "baseline";
            return "invalid";
        }
        json blind_input(const EvaluationSuite &s, const DatasetRegistry &datasets,
                         const CandidateEvaluationRun &b, const CandidateEvaluationRun &c,
                         const std::vector<BlindAssignment> &as, JudgeStage stage, const std::vector<std::string> &only = {},
                         const JudgeBatch *primary = nullptr, const JudgeBatch *secondary = nullptr)
        {
            auto bm = case_map(b), cm = case_map(c);
            json cases = json::array();
            for (const auto &sc : s.cases)
            {
                if (!only.empty() && !has(only, sc.case_id))
                    continue;
                const auto *a = assignment_for(as, sc.case_id);
                json artifacts = json::object();
                artifacts[a->baseline_alias] = bm[sc.case_id]->artifact;
                artifacts[a->candidate_alias] = cm[sc.case_id]->artifact;
                const auto &order = stage == JudgeStage::SecondaryJudging ? a->secondary_order : a->primary_order;
                const auto dataset_case = datasets.case_for_scoring(sc.case_id);
                json item = {{"case_id", sc.case_id}, {"input", dataset_case->input}, {"environment", dataset_case->environment}, {"criterion_ids", sc.criterion_ids}, {"presentation_order", order}, {"artifacts", std::move(artifacts)}};
                if (stage == JudgeStage::Adjudication && primary && secondary)
                {
                    auto pv = std::find_if(primary->verdicts.begin(), primary->verdicts.end(), [&](const auto &v)
                                           { return v.case_id == sc.case_id; });
                    auto sv = std::find_if(secondary->verdicts.begin(), secondary->verdicts.end(), [&](const auto &v)
                                           { return v.case_id == sc.case_id; });
                    item["anonymous_prior_verdicts"] = {{{"winner_alias", pv->winner_alias}, {"scores", pv->scores}, {"evidence_refs", pv->evidence_refs}}, {{"winner_alias", sv->winner_alias}, {"scores", sv->scores}, {"evidence_refs", sv->evidence_refs}}};
                }
                cases.push_back(std::move(item));
            }
            return {{"suite_alias", "blind-suite"}, {"seed", s.seed}, {"cases", std::move(cases)}, {"output_contract", {{"winner_alias", "artifact-A|artifact-B|tie"}, {"score_range", "[0,1]"}, {"exact_case_coverage", true}}}};
        }
        std::optional<JudgeBatch> parse_batch(const json &out, const EvaluationSuite &s, const std::vector<BlindAssignment> &as,
                                              const std::vector<std::string> &expected, const JudgeStageResponse &r, std::string_view role, std::string *error)
        {
            static const std::set<std::string> top = {"verdicts"}, vf = {"case_id", "winner_alias", "scores", "criterion_scores", "confidence", "evidence_refs", "risks"};
            if (!out.is_object() || !contracts::validate_object_fields(out, top, top, contracts::UnknownFieldPolicy::Reject, nullptr, nullptr, "/judge_output") || !out.at("verdicts").is_array())
            {
                if (error)
                    *error = "judge output must satisfy the closed verdict schema";
                return std::nullopt;
            }
            std::set<std::string> want(expected.begin(), expected.end()), seen;
            JudgeBatch batch;
            batch.metadata = s.metadata;
            batch.batch_id = r.manifest.invocation_id.empty() ? std::string(role) + ":batch" : r.manifest.invocation_id + ":batch";
            batch.judge_role = role;
            batch.invocation_id = r.manifest.invocation_id;
            batch.independence_group = r.manifest.independence_group;
            batch.provider = r.manifest.provider;
            batch.model = r.manifest.model;
            try
            {
                for (const auto &x : out.at("verdicts"))
                {
                    if (!contracts::validate_object_fields(x, vf, vf, contracts::UnknownFieldPolicy::Reject, nullptr, nullptr, "/verdict"))
                        throw std::invalid_argument("unknown verdict field");
                    JudgeVerdict v;
                    v.case_id = x.at("case_id").get<std::string>();
                    v.winner_alias = x.at("winner_alias").get<std::string>();
                    v.scores = x.at("scores").get<std::map<std::string, double>>();
                    v.criterion_scores = x.at("criterion_scores").get<std::map<std::string, double>>();
                    v.confidence = x.at("confidence").get<double>();
                    v.evidence_refs = x.at("evidence_refs").get<std::vector<std::string>>();
                    v.risks = x.at("risks").get<std::vector<std::string>>();
                    const auto *a = assignment_for(as, v.case_id);
                    auto sc = std::find_if(s.cases.begin(), s.cases.end(), [&](const auto &q)
                                           { return q.case_id == v.case_id; });
                    if (!a || !want.count(v.case_id) || !seen.insert(v.case_id).second || (v.winner_alias != "tie" && v.winner_alias != "artifact-A" && v.winner_alias != "artifact-B") || v.scores.size() != 2 || !v.scores.count("artifact-A") || !v.scores.count("artifact-B") || v.criterion_scores.size() != sc->criterion_ids.size() || !finite(v.confidence) || v.confidence < 0 || v.confidence > 1)
                        throw std::invalid_argument("verdict case, alias, score or coverage is invalid");
                    for (const auto &[k, q] : v.scores)
                        if (!finite(q) || q < 0 || q > 1)
                            throw std::invalid_argument("artifact score outside [0,1]");
                    for (const auto &id : sc->criterion_ids)
                    {
                        auto q = v.criterion_scores.find(id);
                        if (q == v.criterion_scores.end() || !finite(q->second) || q->second < 0 || q->second > 1)
                            throw std::invalid_argument("criterion score coverage/range invalid");
                    }
                    batch.verdicts.push_back(std::move(v));
                }
                if (seen != want)
                    throw std::invalid_argument("judge output does not exactly cover assigned cases");
            }
            catch (const std::exception &e)
            {
                if (error)
                    *error = e.what();
                return std::nullopt;
            }
            return batch;
        }
        std::vector<std::string> disputes(const JudgeBatch &a, const JudgeBatch &b)
        {
            std::vector<std::string> out;
            for (const auto &x : a.verdicts)
            {
                auto y = std::find_if(b.verdicts.begin(), b.verdicts.end(), [&](const auto &v)
                                      { return v.case_id == x.case_id; });
                if (y == b.verdicts.end() || x.winner_alias != y->winner_alias)
                    out.push_back(x.case_id);
            }
            return out;
        }
        const JudgeVerdict &verdict_for(const JudgeBatch &b, std::string_view id)
        {
            return *std::find_if(b.verdicts.begin(), b.verdicts.end(), [&](const auto &v)
                                 { return v.case_id == id; });
        }
        JudgeCalibrationReport calibrate(const EvaluationSuite &s, const DatasetRegistry &datasets, const std::vector<BlindAssignment> &as, const JudgeBatch &p, const JudgeBatch &q, const std::optional<JudgeBatch> &a)
        {
            JudgeCalibrationReport r;
            r.metadata = s.metadata;
            r.calibration_id = s.suite_id + ":" + std::to_string(s.revision) + ":calibration";
            auto ds = disputes(p, q);
            r.disputed_case_ids = ds;
            const double n = static_cast<double>(s.cases.size());
            std::size_t agree = 0, first = 0, first_n = 0, gt_ok = 0;
            std::map<std::string, double> pc, qc;
            std::vector<double> deltas;
            for (const auto &sc : s.cases)
            {
                const auto &pv = verdict_for(p, sc.case_id);
                const auto &qv = verdict_for(q, sc.case_id);
                const auto *ba = assignment_for(as, sc.case_id);
                auto ps = semantic_winner(pv, *ba), qs = semantic_winner(qv, *ba);
                pc[ps]++;
                qc[qs]++;
                if (ps == qs)
                    agree++;
                if (pv.winner_alias != "tie")
                {
                    first_n++;
                    if (pv.winner_alias == ba->primary_order.front())
                        first++;
                }
                if (qv.winner_alias != "tie")
                {
                    first_n++;
                    if (qv.winner_alias == ba->secondary_order.front())
                        first++;
                }
                deltas.push_back((pv.scores.at(ba->candidate_alias) - pv.scores.at(ba->baseline_alias) + qv.scores.at(ba->candidate_alias) - qv.scores.at(ba->baseline_alias)) / 2.0);
                const JudgeVerdict *fv = &pv;
                if (has(ds, sc.case_id) && a)
                    fv = &verdict_for(*a, sc.case_id);
                auto final = semantic_winner(*fv, *ba);
                if (final == "candidate")
                    r.candidate_win_rate += 1;
                auto dc = datasets.case_for_scoring(sc.case_id);
                if (dc && dc->ground_truth.is_object() && dc->ground_truth.contains("preferred_revision") && dc->ground_truth.at("preferred_revision").is_string())
                {
                    r.ground_truth_samples++;
                    if (dc->ground_truth.at("preferred_revision").get<std::string>() == final)
                        gt_ok++;
                }
            }
            r.raw_agreement = n ? agree / n : 0;
            double pe = 0;
            for (const auto &k : {"baseline", "candidate", "tie"})
                pe += (pc[k] / n) * (qc[k] / n);
            r.cohens_kappa = (1 - pe) > 1e-12 ? (r.raw_agreement - pe) / (1 - pe) : 1;
            r.first_position_win_rate = first_n ? static_cast<double>(first) / first_n : 0;
            r.candidate_win_rate = n ? r.candidate_win_rate / n : 0;
            r.ground_truth_accuracy = r.ground_truth_samples ? static_cast<double>(gt_ok) / r.ground_truth_samples : 0;
            double mean = deltas.empty() ? 0 : std::accumulate(deltas.begin(), deltas.end(), 0.0) / deltas.size();
            for (double x : deltas)
                r.score_delta_variance += (x - mean) * (x - mean);
            if (deltas.size() > 1)
                r.score_delta_variance /= deltas.size() - 1;
            double z = 1.96, den = 1 + z * z / n, center = (r.candidate_win_rate + z * z / (2 * n)) / den, half = z * std::sqrt((r.candidate_win_rate * (1 - r.candidate_win_rate) + z * z / (4 * n)) / n) / den;
            r.candidate_win_ci95_low = std::max(0.0, center - half);
            r.candidate_win_ci95_high = std::min(1.0, center + half);
            if (r.raw_agreement < s.minimum_judge_agreement)
                r.calibration_findings.push_back("judge_agreement_below_threshold");
            if (r.ground_truth_samples == 0)
                r.calibration_findings.push_back("ground_truth_calibration_missing");
            else if (r.ground_truth_accuracy < s.minimum_ground_truth_accuracy)
                r.calibration_findings.push_back("ground_truth_accuracy_below_threshold");
            if (std::abs(r.first_position_win_rate - 0.5) > 0.25)
                r.calibration_findings.push_back("possible_position_bias");
            return r;
        }
        QualityMetricReport metric_report(const EvaluationSuite &s, const CandidateEvaluationRun &b, const CandidateEvaluationRun &c)
        {
            QualityMetricReport r;
            r.metadata = s.metadata;
            r.report_id = s.suite_id + ":" + std::to_string(s.revision) + ":metrics";
            auto bm = case_map(b), cm = case_map(c);
            for (const auto &p : s.metric_policies)
            {
                std::vector<double> bv, cv, dv;
                bool missing = false, flaky = false, critical = false;
                for (const auto &sc : s.cases)
                {
                    if (sc.layer != p.layer)
                        continue;
                    auto bi = bm[sc.case_id]->metrics.find(p.metric), ci = cm[sc.case_id]->metrics.find(p.metric);
                    if (bi == bm[sc.case_id]->metrics.end() || ci == cm[sc.case_id]->metrics.end())
                    {
                        missing = true;
                        continue;
                    }
                    bv.push_back(bi->second);
                    cv.push_back(ci->second);
                    dv.push_back(ci->second - bi->second);
                    if (p.critical_zero && ci->second != 0)
                        critical = true;
                    auto si = cm[sc.case_id]->metric_samples.find(p.metric);
                    if (si != cm[sc.case_id]->metric_samples.end() && si->second.size() > 1 && *std::max_element(si->second.begin(), si->second.end()) - *std::min_element(si->second.begin(), si->second.end()) > s.maximum_flaky_delta)
                        flaky = true;
                }
                if (missing || bv.empty())
                {
                    r.missing_metrics.push_back(p.metric);
                    continue;
                }
                MetricComparison x;
                x.metric = p.metric;
                x.baseline_mean = std::accumulate(bv.begin(), bv.end(), 0.0) / bv.size();
                x.candidate_mean = std::accumulate(cv.begin(), cv.end(), 0.0) / cv.size();
                x.paired_delta = std::accumulate(dv.begin(), dv.end(), 0.0) / dv.size();
                if (dv.size() > 1)
                {
                    double sum = 0;
                    for (double d : dv)
                        sum += (d - x.paired_delta) * (d - x.paired_delta);
                    x.ci95_half_width = 1.96 * std::sqrt(sum / (dv.size() - 1)) / std::sqrt(static_cast<double>(dv.size()));
                }
                x.regression = p.higher_is_better ? x.paired_delta + x.ci95_half_width < -p.maximum_regression : x.paired_delta - x.ci95_half_width > p.maximum_regression;
                r.comparisons.push_back(x);
                r.candidate_means[p.metric] = x.candidate_mean;
                if (flaky)
                    r.flaky_metrics.push_back(p.metric);
                if (critical)
                    r.critical_violations.push_back(p.metric + ":critical_zero");
                if (p.minimum_candidate_value && x.candidate_mean < *p.minimum_candidate_value)
                    r.critical_violations.push_back(p.metric + ":minimum_candidate_value");
            }
            return r;
        }
    } // namespace

    RoleRuntimeJudgeModel::RoleRuntimeJudgeModel(std::shared_ptr<llm_runtime::RoleRuntime> runtime) : runtime_(std::move(runtime))
    {
        if (!runtime_)
            throw std::invalid_argument("RoleRuntime is required");
    }
    bool RoleRuntimeJudgeModel::bind(JudgeStage stage, JudgeRoleBinding binding)
    {
        if (!semantic(stage) || binding.profile_id.empty() || binding.profile_revision.empty())
            return false;
        return bindings_.emplace(stage, std::move(binding)).second;
    }
    JudgeStageResponse RoleRuntimeJudgeModel::invoke(const JudgeStageRequest &r)
    {
        JudgeStageResponse out;
        if (r.cancelled && r.cancelled())
        {
            out.error_code = "judge_cancelled";
            out.error_message = "cancelled before invocation";
            return out;
        }
        auto b = bindings_.find(r.stage);
        if (b == bindings_.end())
        {
            out.error_code = "judge_role_unbound";
            out.error_message = "no RoleRuntime profile is bound to " + judge_stage_name(r.stage);
            return out;
        }
        auto input = contracts::canonical_json(r.input);
        llm_runtime::RoleInvocationRequest q;
        q.metadata = r.metadata;
        q.invocation_id = r.workflow_id + ":" + judge_stage_name(r.stage) + ":" + std::to_string(r.attempt);
        q.trace_id = r.metadata.identity.run_id.empty() ? r.workflow_id : r.metadata.identity.run_id;
        q.profile_id = b->second.profile_id;
        q.profile_revision = b->second.profile_revision;
        q.prompt_variables = {{"input", input}};
        q.input.context = memory_context(r.memory_view).dump();
        q.memory_view = {r.memory_view.snapshot.snapshot_id, "evaluation", r.memory_view.manifest.view_digest};
        for (const auto &cap : b->second.granted_capabilities)
            if (has(r.granted_capabilities, cap))
                q.granted_capabilities.push_back(cap);
        q.independence = r.independence;
        q.required_region = b->second.required_region;
        q.estimated_input_tokens = (input.size() + q.input.context.size() + 3) / 4;
        q.policy_revision = "phase4-v2-f6e-r1";
        auto rr = runtime_->invoke(std::move(q));
        out.manifest = rr.manifest;
        out.error_code = rr.error_code;
        out.error_message = rr.error_message;
        if (rr.ok && rr.structured_output)
        {
            out.ok = true;
            out.output = *rr.structured_output;
        }
        return out;
    }

    LLMJudgeWorkflow::LLMJudgeWorkflow(memory_v2::MemoryViewEngine &views, JudgeStore &store, JudgeStageModel &model, approval::PolicyDecisionPoint policy) : views_(views), store_(store), model_(model), policy_(std::move(policy)) {}
    JudgeWorkflowResult LLMJudgeWorkflow::run(const EvaluationSuite &s, const DatasetRegistry &datasets,
                                              const CandidateEvaluationRun &b, const CandidateEvaluationRun &c, const memory_v2::MemoryScope &subject,
                                              const JudgeWorkflowOptions &o)
    {
        JudgeWorkflowResult result;
        auto now = o.now ? o.now : now_utc;
        std::string error;
        if (!validate_inputs(s, datasets, b, c, subject, &error))
        {
            result.error_code = "judge_input_invalid";
            result.error_message = error;
            return result;
        }
        auto sd = digest(encode(s)), bd = digest(encode(b)), cd = digest(encode(c));
        auto workflow = o.workflow_id.empty()
                            ? s.metadata.identity.task_id + ":judge:" + s.suite_id
                            : o.workflow_id;
        auto view = views_.build(memory_v2::make_view_spec(memory_v2::MemoryViewMode::Evaluation, s.metadata, subject), now());
        if (view.fail_closed)
        {
            result.state = JudgeWorkflowState::ManualReview;
            result.error_code = "evaluation_view_failed";
            result.error_message = view.error;
            return result;
        }
        if (std::any_of(view.records.begin(), view.records.end(), [](const auto &record)
                        { return protected_artifact(record.content); }))
        {
            result.state = JudgeWorkflowState::ManualReview;
            result.error_code = "evaluation_view_blindness_violation";
            result.error_message = "Evaluation Memory View contains protected candidate or label identity";
            return result;
        }
        auto finish = [&](JudgeCheckpoint cp)
        {result.state=cp.state;result.checkpoint=std::move(cp);result.error_code=result.checkpoint.error_code;result.error_message=result.checkpoint.error_message;if(auto report=store_.load_report(s.metadata.identity.tenant_id,workflow))result.report=report->report;return result; };
        auto stored = store_.load(s.metadata.identity.tenant_id, workflow);
        JudgeCheckpoint cp;
        std::uint64_t rev = 0;
        if (stored)
        {
            cp = stored->checkpoint;
            rev = stored->revision;
            if (cp.suite_digest != sd || cp.baseline_run_digest != bd || cp.candidate_run_digest != cd || cp.memory_snapshot_id != view.snapshot.snapshot_id || cp.memory_view_digest != view.manifest.view_digest)
            {
                cp.state = JudgeWorkflowState::ManualReview;
                cp.error_code = "judge_input_digest_mismatch";
                cp.error_message = "durable evaluation inputs or pinned Evaluation Memory View changed";
                return finish(cp);
            }
            if (final_state(cp.state))
                return finish(cp);
        }
        else
        {
            cp.metadata = s.metadata;
            cp.workflow_id = workflow;
            cp.suite_digest = sd;
            cp.baseline_run_digest = bd;
            cp.candidate_run_digest = cd;
            cp.memory_snapshot_id = view.snapshot.snapshot_id;
            cp.memory_view_digest = view.manifest.view_digest;
            cp.updated_at = now();
            auto commit = store_.create(cp);
            if (!commit)
            {
                result.error_code = "judge_checkpoint_create_failed";
                result.error_message = commit.error;
                return result;
            }
            rev = commit.revision;
        }
        auto persist = [&]()
        {cp.updated_at=now();cp.revision=rev+1;auto commit=store_.compare_exchange(cp,rev);if(!commit)throw std::runtime_error("judge checkpoint CAS failed: "+commit.error);rev=commit.revision; };
        auto complete = [&](JudgeStage a, JudgeStage next)
        {cp.completed_stages.push_back(judge_stage_name(a));cp.next_stage=next;persist(); };
        auto manual = [&](std::string code, std::string message)
        {cp.state=JudgeWorkflowState::ManualReview;cp.error_code=std::move(code);cp.error_message=std::move(message);persist();return finish(cp); };
        auto cancelled = [&]()
        {if(!o.cancelled||!o.cancelled())return false;cp.state=JudgeWorkflowState::Cancelled;cp.error_code="judge_cancelled";cp.error_message="evaluation workflow cancelled";persist();return true; };
        auto invoke = [&](JudgeStage stage, json input, llm_runtime::IndependenceRequirement independence) -> std::optional<JudgeStageResponse>
        {auto key=judge_stage_name(stage);auto&attempt=cp.stage_attempts[key];if(attempt>=o.max_stage_attempts)return std::nullopt;++attempt;persist();JudgeStageRequest request{s.metadata,workflow,stage,attempt,view,std::move(input),std::move(independence),{},o.cancelled};auto response=model_.invoke(request);if(response.ok){auto t=tokens(response.manifest);auto cost_usd=cost(response.manifest);cp.consumed_tokens+=t;cp.consumed_cost_usd+=cost_usd;cp.artifacts.push_back({stage,attempt,response.manifest.invocation_id,response.manifest.output_digest.empty()?digest(response.output):response.manifest.output_digest,response.manifest.provider,response.manifest.model,response.manifest.independence_group,t,cost_usd,response.output});if(cp.consumed_tokens>o.token_budget||cp.consumed_cost_usd>o.cost_limit_usd||(!o.deadline.empty()&&now()>o.deadline))return std::nullopt;}return response; };
        try
        {
            while (true)
            {
                if (cancelled())
                    return finish(cp);
                if (!o.deadline.empty() && now() > o.deadline)
                    return manual("judge_budget_exhausted", "evaluation deadline expired");
                if (cp.next_stage == JudgeStage::Preparation)
                {
                    cp.assignments = assignments(s, b, c);
                    complete(JudgeStage::Preparation, JudgeStage::PrimaryJudging);
                    continue;
                }
                if (cp.next_stage == JudgeStage::PrimaryJudging)
                {
                    std::vector<std::string> ids;
                    for (const auto &x : s.cases)
                        ids.push_back(x.case_id);
                    auto response = invoke(JudgeStage::PrimaryJudging, blind_input(s, datasets, b, c, cp.assignments, JudgeStage::PrimaryJudging), {});
                    if (!response)
                        return manual("judge_budget_exhausted", "primary judge budget exhausted");
                    if (!response->ok)
                    {
                        if (cp.stage_attempts["primary_judging"] >= o.max_stage_attempts)
                            return manual(response->error_code.empty() ? "primary_judge_failed" : response->error_code, response->error_message);
                        continue;
                    }
                    std::string parse_error;
                    auto batch = parse_batch(response->output, s, cp.assignments, ids, *response, "primary_judge", &parse_error);
                    if (!batch)
                    {
                        if (cp.stage_attempts["primary_judging"] >= o.max_stage_attempts)
                            return manual("primary_verdict_invalid", parse_error);
                        continue;
                    }
                    cp.primary = std::move(*batch);
                    complete(JudgeStage::PrimaryJudging, JudgeStage::SecondaryJudging);
                    continue;
                }
                if (cp.next_stage == JudgeStage::SecondaryJudging)
                {
                    std::vector<std::string> ids;
                    for (const auto &x : s.cases)
                        ids.push_back(x.case_id);
                    llm_runtime::IndependenceRequirement ind;
                    ind.forbidden_groups = o.forbidden_independence_groups;
                    ind.forbidden_providers = o.forbidden_providers;
                    ind.forbidden_models = o.forbidden_models;
                    if (!cp.primary->independence_group.empty())
                        ind.forbidden_groups.push_back(cp.primary->independence_group);
                    if (!cp.primary->provider.empty())
                        ind.forbidden_providers.push_back(cp.primary->provider);
                    if (!cp.primary->model.empty())
                        ind.forbidden_models.push_back(cp.primary->model);
                    ind.require_provider_diversity = o.require_provider_diversity;
                    ind.require_model_diversity = o.require_model_diversity;
                    auto response = invoke(JudgeStage::SecondaryJudging, blind_input(s, datasets, b, c, cp.assignments, JudgeStage::SecondaryJudging), ind);
                    if (!response)
                        return manual("judge_budget_exhausted", "secondary judge budget exhausted");
                    if (!response->ok)
                    {
                        if (cp.stage_attempts["secondary_judging"] >= o.max_stage_attempts)
                            return manual(response->error_code.empty() ? "secondary_judge_failed" : response->error_code, response->error_message);
                        continue;
                    }
                    if ((o.require_provider_diversity && (response->manifest.provider.empty() || response->manifest.provider == cp.primary->provider)) || (o.require_model_diversity && (response->manifest.model.empty() || response->manifest.model == cp.primary->model)) || (!response->manifest.independence_group.empty() && response->manifest.independence_group == cp.primary->independence_group))
                    {
                        return manual("judge_independence_violation", "secondary judge did not satisfy provider/model/group diversity");
                    }
                    std::string parse_error;
                    auto batch = parse_batch(response->output, s, cp.assignments, ids, *response, "secondary_judge", &parse_error);
                    if (!batch)
                    {
                        if (cp.stage_attempts["secondary_judging"] >= o.max_stage_attempts)
                            return manual("secondary_verdict_invalid", parse_error);
                        continue;
                    }
                    cp.secondary = std::move(*batch);
                    auto ds = disputes(*cp.primary, *cp.secondary);
                    complete(JudgeStage::SecondaryJudging, ds.empty() ? JudgeStage::Calibration : JudgeStage::Adjudication);
                    continue;
                }
                if (cp.next_stage == JudgeStage::Adjudication)
                {
                    auto ds = disputes(*cp.primary, *cp.secondary);
                    llm_runtime::IndependenceRequirement ind;
                    ind.forbidden_groups = o.forbidden_independence_groups;
                    ind.forbidden_groups.push_back(cp.primary->independence_group);
                    ind.forbidden_groups.push_back(cp.secondary->independence_group);
                    ind.forbidden_providers = {cp.primary->provider, cp.secondary->provider};
                    ind.forbidden_models = {cp.primary->model, cp.secondary->model};
                    ind.require_provider_diversity = o.require_provider_diversity;
                    ind.require_model_diversity = o.require_model_diversity;
                    auto response = invoke(JudgeStage::Adjudication, blind_input(s, datasets, b, c, cp.assignments, JudgeStage::Adjudication, ds, &*cp.primary, &*cp.secondary), ind);
                    if (!response)
                        return manual("judge_budget_exhausted", "adjudicator budget exhausted");
                    if (!response->ok)
                    {
                        if (cp.stage_attempts["adjudication"] >= o.max_stage_attempts)
                            return manual(response->error_code.empty() ? "adjudicator_failed" : response->error_code, response->error_message);
                        continue;
                    }
                    if ((o.require_provider_diversity && (response->manifest.provider.empty() || response->manifest.provider == cp.primary->provider || response->manifest.provider == cp.secondary->provider)) || (o.require_model_diversity && (response->manifest.model.empty() || response->manifest.model == cp.primary->model || response->manifest.model == cp.secondary->model)))
                        return manual("adjudicator_independence_violation", "adjudicator did not satisfy configured diversity");
                    std::string parse_error;
                    auto batch = parse_batch(response->output, s, cp.assignments, ds, *response, "adjudicator", &parse_error);
                    if (!batch)
                    {
                        if (cp.stage_attempts["adjudication"] >= o.max_stage_attempts)
                            return manual("adjudication_invalid", parse_error);
                        continue;
                    }
                    cp.adjudication = std::move(*batch);
                    complete(JudgeStage::Adjudication, JudgeStage::Calibration);
                    continue;
                }
                if (cp.next_stage == JudgeStage::Calibration)
                {
                    cp.calibration = calibrate(s, datasets, cp.assignments, *cp.primary, *cp.secondary, cp.adjudication);
                    complete(JudgeStage::Calibration, JudgeStage::Metrics);
                    continue;
                }
                if (cp.next_stage == JudgeStage::Metrics)
                {
                    cp.metrics = metric_report(s, b, c);
                    complete(JudgeStage::Metrics, JudgeStage::UpgradeGate);
                    continue;
                }
                if (cp.next_stage == JudgeStage::UpgradeGate)
                {
                    UpgradeDecision d;
                    d.metadata = s.metadata;
                    d.decision_id = workflow + ":upgrade";
                    d.baseline_revision = b.revision_id;
                    d.candidate_revision = c.revision_id;
                    d.rollback_revision = b.revision_id;
                    d.calibration_digest = digest(encode(*cp.calibration));
                    d.metric_report_digest = digest(encode(*cp.metrics));
                    bool regression = std::any_of(cp.metrics->comparisons.begin(), cp.metrics->comparisons.end(), [](const auto &x)
                                                  { return x.regression; });
                    if (regression)
                        d.reasons.push_back("deterministic_metric_regression");
                    if (!cp.metrics->critical_violations.empty())
                        d.reasons.push_back("critical_metric_violation");
                    if (regression || !cp.metrics->critical_violations.empty())
                        d.outcome = UpgradeOutcome::Rejected;
                    else if (!cp.metrics->missing_metrics.empty() || !cp.metrics->flaky_metrics.empty() || cp.calibration->raw_agreement < s.minimum_judge_agreement || cp.calibration->ground_truth_samples == 0 || cp.calibration->ground_truth_accuracy < s.minimum_ground_truth_accuracy)
                    {
                        d.outcome = UpgradeOutcome::ManualReview;
                        if (!cp.metrics->missing_metrics.empty())
                            d.reasons.push_back("required_metrics_missing");
                        if (!cp.metrics->flaky_metrics.empty())
                            d.reasons.push_back("flaky_metrics_detected");
                        if (cp.calibration->raw_agreement < s.minimum_judge_agreement)
                            d.reasons.push_back("judge_agreement_below_threshold");
                        if (cp.calibration->ground_truth_samples == 0)
                            d.reasons.push_back("ground_truth_calibration_missing");
                        else if (cp.calibration->ground_truth_accuracy < s.minimum_ground_truth_accuracy)
                            d.reasons.push_back("ground_truth_accuracy_below_threshold");
                    }
                    else
                    {
                        approval::PolicyContext ctx;
                        ctx.identity = s.metadata.identity;
                        ctx.actor_id = "judge.workflow";
                        ctx.action = "profile.upgrade";
                        ctx.resource = c.revision_id;
                        ctx.effect_class = "none";
                        ctx.risk_level = "high";
                        ctx.requester_id = ctx.actor_id;
                        auto pe = policy_.evaluate(ctx);
                        if (pe.outcome == approval::PolicyOutcome::Deny)
                        {
                            d.outcome = UpgradeOutcome::Rejected;
                            d.reasons = pe.reasons;
                        }
                        else
                        {
                            d.approval_request_digest = digest(json{{"suite_digest", sd}, {"baseline_run_digest", bd}, {"candidate_run_digest", cd}, {"calibration_digest", d.calibration_digest}, {"metric_report_digest", d.metric_report_digest}, {"baseline_revision", b.revision_id}, {"candidate_revision", c.revision_id}, {"policy_revision", policy_.rules().revision}});
                            bool approval = o.require_upgrade_approval || pe.outcome == approval::PolicyOutcome::RequireApproval;
                            if (approval && (o.approval_decision_id.empty() || !o.approval_validator || !o.approval_validator(d.approval_request_digest, o.approval_decision_id)))
                            {
                                d.outcome = UpgradeOutcome::AwaitingApproval;
                                d.reasons.push_back("bound_upgrade_approval_required");
                            }
                            else
                            {
                                d.outcome = UpgradeOutcome::Approved;
                                d.approval_decision_id = o.approval_decision_id;
                            }
                        }
                    }
                    cp.decision = d;
                    if (d.outcome == UpgradeOutcome::AwaitingApproval)
                    {
                        cp.state = JudgeWorkflowState::AwaitingApproval;
                        cp.error_code = "upgrade_approval_required";
                        cp.error_message = "candidate passed gates but requires a bound approval";
                        persist();
                        return finish(cp);
                    }
                    cp.state = d.outcome == UpgradeOutcome::Approved ? JudgeWorkflowState::Approved : d.outcome == UpgradeOutcome::Rejected ? JudgeWorkflowState::Rejected
                                                                                                                                            : JudgeWorkflowState::ManualReview;
                    cp.next_stage = JudgeStage::Complete;
                    cp.completed_stages.push_back(judge_stage_name(JudgeStage::UpgradeGate));
                    EvaluationReport report;
                    report.metadata = s.metadata;
                    report.workflow_id = workflow;
                    report.run_kind = o.run_kind;
                    report.executed = b.executed && c.executed;
                    report.suite_digest = sd;
                    report.baseline_run_digest = bd;
                    report.candidate_run_digest = cd;
                    report.primary = *cp.primary;
                    report.secondary = *cp.secondary;
                    report.adjudication = cp.adjudication;
                    report.calibration = *cp.calibration;
                    report.metrics = *cp.metrics;
                    report.decision = *cp.decision;
                    report.trend_key = s.suite_id + ":" + s.dataset_version;
                    report.created_at = now();
                    cp.evaluation_report_digest = digest(encode(report));
                    cp.updated_at = now();
                    cp.revision = rev + 1;
                    auto commit = store_.commit_report(cp, rev, report);
                    if (!commit)
                        throw std::runtime_error("terminal evaluation commit failed: " + commit.error);
                    rev = commit.revision;
                    result.report = report;
                    return finish(cp);
                }
                if (cp.next_stage == JudgeStage::Complete)
                    return finish(cp);
            }
        }
        catch (const std::exception &e)
        {
            result.state = JudgeWorkflowState::Failed;
            result.checkpoint = cp;
            result.error_code = "judge_workflow_error";
            result.error_message = e.what();
            return result;
        }
    }

} // namespace agent_framework::eval
