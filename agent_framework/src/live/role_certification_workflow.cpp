#include "agent/live/role_certification.hpp"

#include <algorithm>
#include <stdexcept>

namespace agent_framework::live
{
    namespace
    {
        bool terminal(RoleCertificationState s) { return s != RoleCertificationState::Running && s != RoleCertificationState::AwaitingApproval; }
        bool all_required_executed(const RoleLiveMatrix &m, const std::vector<LiveCellResult> &r)
        {
            for (const auto &c : m.cells)
                if (c.required)
                {
                    auto it = std::find_if(r.begin(), r.end(), [&](const auto &x)
                                           { return x.cell_id == c.cell_id; });
                    if (it == r.end() || !it->executed)
                        return false;
                }
            return true;
        }
        std::uint64_t tokens(const llm_runtime::UsageRecord &u) { return u.input_tokens.value_or(0) + u.output_tokens.value_or(0); }
        void notify(const RoleCertificationOptions &o, std::string_view severity, std::string_view message)
        {
            if (o.alert)
                o.alert(severity, message);
        }
    }

    RoleRuntimeLiveCellExecutor::RoleRuntimeLiveCellExecutor(std::shared_ptr<llm_runtime::RoleRuntime> runtime) : runtime_(std::move(runtime))
    {
        if (!runtime_)
            throw std::invalid_argument("RoleRuntime is required");
    }
    bool RoleRuntimeLiveCellExecutor::bind(std::string id, RoleRuntimeCellBinding binding)
    {
        if (id.empty() || binding.request.profile_id.empty())
            return false;
        return bindings_.emplace(std::move(id), std::move(binding)).second;
    }
    LiveCellResult RoleRuntimeLiveCellExecutor::execute(const LiveCellExecutionRequest &r)
    {
        LiveCellResult out;
        out.cell_id = r.cell.cell_id;
        out.spec_digest = live_cell_spec_digest(r.cell);
        auto it = bindings_.find(r.cell.cell_id);
        if (it == bindings_.end())
        {
            out.reason = "role_runtime_binding_missing";
            out.error_class = "configuration";
            return out;
        }
        auto request = it->second.request;
        if (request.metadata.identity.tenant_id.empty())
            request.metadata = r.matrix.metadata;
        if (request.required_region.empty())
            request.required_region = r.cell.region;
        auto result = runtime_->invoke(std::move(request));
        const auto &m = result.manifest;
        out.executed = !m.invocation_id.empty();
        out.outcome = result.ok ? LiveCellOutcome::Passed : (out.executed ? LiveCellOutcome::Failed : LiveCellOutcome::Inconclusive);
        out.reason = result.error_message;
        out.error_class = result.error_code;
        out.invocation_id = m.invocation_id;
        if (out.executed)
            out.invocation_manifest_digest = llm_runtime::encode(m).at("canonical_digest");
        out.role = m.role;
        out.profile_id = m.profile_id;
        out.profile_revision = m.profile_revision;
        out.prompt_id = m.prompt_id;
        out.prompt_revision = m.prompt_revision;
        out.provider = m.provider;
        out.model = m.model;
        out.independence_group = m.independence_group;
        out.region = r.cell.region;
        out.capabilities = m.capabilities;
        out.evidence_digests = it->second.evidence_digests;
        if (!m.output_digest.empty())
            out.evidence_digests.push_back(m.output_digest);
        out.oracle_digests = it->second.oracle_digests;
        out.read_only = it->second.read_only;
        out.blind = it->second.blind;
        out.latency_ms = m.latency_ms;
        out.tokens = tokens(m.usage);
        out.cost_usd = m.usage.cost_usd.value_or(0.0);
        out.started_at = m.started_at;
        out.finished_at = m.finished_at;
        out.fallback_used = std::any_of(m.attempts.begin(), m.attempts.end(), [](const auto &a)
                                        { return a.fallback; });
        out.recovered = r.cell.failure_scenario != LiveFailureScenario::None && result.ok;
        return out;
    }

    RoleLiveCertificationWorkflow::RoleLiveCertificationWorkflow(RoleCertificationStore &store, LiveCellExecutor &executor) : store_(store), executor_(executor) {}
    RoleCertificationResult RoleLiveCertificationWorkflow::run(const LiveEnvironmentProfile &e, const RoleLiveMatrix &m, const RoleCertificationOptions &o)
    {
        RoleCertificationResult result;
        auto contract_errors = validate_live_contract(e, m);
        if (o.workflow_id.empty())
        {
            result.error_code = "workflow_id_missing";
            result.error_message = "live certification workflow id is required";
            return result;
        }
        if (!contract_errors.empty())
        {
            result.state = RoleCertificationState::Failed;
            result.error_code = "contract_invalid";
            result.error_message = contract_errors.front();
            return result;
        }
        const auto ed = role_environment_digest(e), md = role_live_matrix_digest(m);
        auto stored = store_.load(e.metadata.identity.tenant_id, o.workflow_id);
        RoleCertificationCheckpoint cp;
        if (stored)
        {
            cp = stored->checkpoint;
            if (cp.environment_digest != ed || cp.matrix_digest != md)
            {
                result.state = RoleCertificationState::Failed;
                result.checkpoint = cp;
                result.error_code = "immutable_binding_mismatch";
                result.error_message = "restart attempted with different environment or matrix";
                return result;
            }
            if (terminal(cp.state))
            {
                result.state = cp.state;
                result.checkpoint = cp;
                auto report = store_.load_report(e.metadata.identity.tenant_id, o.workflow_id);
                if (report)
                    result.report = report->report;
                return result;
            }
        }
        else
        {
            cp.metadata = e.metadata;
            cp.workflow_id = o.workflow_id;
            cp.environment_digest = ed;
            cp.matrix_digest = md;
            cp.updated_at = o.now;
            if (!store_.create(cp))
            {
                result.error_code = "checkpoint_create_failed";
                result.error_message = "unable to create live checkpoint";
                return result;
            }
            stored = store_.load(e.metadata.identity.tenant_id, o.workflow_id);
        }
        auto persist = [&]()
        {auto expected=cp.revision;cp.revision++;cp.updated_at=o.now;auto c=store_.compare_exchange(cp,expected);return static_cast<bool>(c); };
        for (; cp.next_cell < m.cells.size();)
        {
            if (o.cancelled && o.cancelled())
            {
                cp.state = RoleCertificationState::Cancelled;
                cp.error_code = "cancelled";
                cp.error_message = "live certification cancelled";
                break;
            }
            const auto &spec = m.cells[cp.next_cell];
            bool deps = true;
            for (const auto &d : spec.dependencies)
            {
                auto it = std::find_if(cp.cells.begin(), cp.cells.end(), [&](const auto &x)
                                       { return x.cell_id == d; });
                if (it == cp.cells.end() || it->outcome != LiveCellOutcome::Passed)
                {
                    deps = false;
                    break;
                }
            }
            LiveCellResult cell;
            if (!deps)
            {
                cell.cell_id = spec.cell_id;
                cell.spec_digest = live_cell_spec_digest(spec);
                cell.reason = "dependency_not_passed";
                cell.error_class = "dependency";
            }
            else
            {
                try
                {
                    cell = executor_.execute({e, m, spec, cp.cells});
                }
                catch (const std::exception &ex)
                {
                    cell.cell_id = spec.cell_id;
                    cell.spec_digest = live_cell_spec_digest(spec);
                    cell.reason = ex.what();
                    cell.error_class = "executor_exception";
                }
                catch (...)
                {
                    cell.cell_id = spec.cell_id;
                    cell.spec_digest = live_cell_spec_digest(spec);
                    cell.reason = "unknown executor exception";
                    cell.error_class = "executor_exception";
                }
            }
            cp.cells.push_back(std::move(cell));
            cp.next_cell++;
            if (!persist())
            {
                result.state = RoleCertificationState::Failed;
                result.checkpoint = cp;
                result.error_code = "checkpoint_commit_failed";
                result.error_message = "live cell checkpoint CAS failed";
                return result;
            }
            if (spec.required && (!cp.cells.back().executed || cp.cells.back().outcome == LiveCellOutcome::Inconclusive))
            {
                cp.state = RoleCertificationState::Inconclusive;
                break;
            }
        }
        RoleCertificationReport report;
        report.metadata = e.metadata;
        report.workflow_id = o.workflow_id;
        report.environment_digest = ed;
        report.matrix_digest = md;
        report.cells = cp.cells;
        report.executed = all_required_executed(m, cp.cells);
        report.issued_at = o.now;
        report.expires_at = e.expires_at;
        report.approval_decision_id = o.approval_decision_id;
        report.blockers = validate_live_results(e, m, cp.cells);
        if (e.endpoint_class == "production")
        {
            if (!o.cell_evidence_verifier)
                report.blockers.push_back("cell_evidence_verifier_missing");
            else
                for (const auto &spec : m.cells)
                {
                    const auto found = std::find_if(cp.cells.begin(), cp.cells.end(),
                        [&](const auto &cell) { return cell.cell_id == spec.cell_id; });
                    if (found == cp.cells.end() || !found->executed) continue;
                    std::string verification_error;
                    if (!o.cell_evidence_verifier(e, spec, *found, &verification_error))
                        report.blockers.push_back("cell_evidence_attestation_invalid:" +
                            spec.cell_id + (verification_error.empty() ? std::string() :
                            ":" + verification_error));
                }
        }
        if (!e.expires_at.empty() && !o.now.empty() && e.expires_at < o.now)
            report.blockers.push_back("certification_expired_before_issue");
        double total_cost = 0, total_tokens = 0, total_latency = 0;
        for (const auto &c : cp.cells)
        {
            total_cost += c.cost_usd;
            total_tokens += c.tokens;
            total_latency += c.latency_ms;
        }
        report.metrics = {{"executed_cells", static_cast<double>(std::count_if(cp.cells.begin(), cp.cells.end(), [](const auto &c)
                                                                               { return c.executed; }))},
                          {"required_cells", static_cast<double>(std::count_if(m.cells.begin(), m.cells.end(), [](const auto &c)
                                                                               { return c.required; }))},
                          {"total_cost_usd", total_cost},
                          {"total_tokens", total_tokens},
                          {"total_latency_ms", total_latency}};
        if (cp.state == RoleCertificationState::Cancelled)
            report.state = cp.state;
        else if (!report.blockers.empty() || !report.executed)
            report.state = RoleCertificationState::Inconclusive;
        else if (o.approval_decision_id.empty() || !o.approval_validator)
            report.state = RoleCertificationState::AwaitingApproval;
        else
            report.state = RoleCertificationState::Certified;
        auto signing_digest = role_report_signing_digest(report);
        if (report.state == RoleCertificationState::Certified && !o.approval_validator(signing_digest, o.approval_decision_id))
        {
            report.state = RoleCertificationState::Rejected;
            report.blockers.push_back("approval_rejected");
            signing_digest = role_report_signing_digest(report);
        }
        if (!o.signer)
        {
            if (report.state == RoleCertificationState::Certified)
            {
                report.state = RoleCertificationState::Inconclusive;
                report.blockers.push_back("report_signer_missing");
                signing_digest = role_report_signing_digest(report);
            }
        }
        else
        {
            report.signature = o.signer(signing_digest);
            if (report.signature.signed_digest != signing_digest || report.signature.algorithm.empty() || report.signature.key_id.empty() || report.signature.signature.empty() || !o.signature_verifier || !o.signature_verifier(report.signature))
            {
                report.state = RoleCertificationState::Inconclusive;
                report.blockers.push_back("report_signature_invalid");
                signing_digest = role_report_signing_digest(report);
                report.signature = o.signer(signing_digest);
            }
        }
        cp.state = report.state;
        cp.blockers = report.blockers;
        cp.report_digest = encode(report).at("canonical_digest");
        auto expected = cp.revision;
        cp.revision++;
        cp.updated_at = o.now;
        if (report.state == RoleCertificationState::AwaitingApproval)
        {
            auto pending = store_.compare_exchange(cp, expected);
            if (!pending)
            {
                result.state = RoleCertificationState::Failed;
                result.checkpoint = cp;
                result.error_code = "approval_checkpoint_commit_failed";
                result.error_message = pending.error;
                return result;
            }
            notify(o, "warning", "live certification awaits approval");
            result.state = report.state;
            result.checkpoint = cp;
            result.report = report;
            return result;
        }
        auto committed = store_.commit_report(cp, expected, report);
        if (!committed)
        {
            result.state = RoleCertificationState::Failed;
            result.checkpoint = cp;
            result.error_code = "report_commit_failed";
            result.error_message = committed.error;
            return result;
        }
        if (report.state == RoleCertificationState::Inconclusive || report.state == RoleCertificationState::Rejected)
            notify(o, "error", report.blockers.empty() ? "live certification did not pass" : report.blockers.front());
        result.state = report.state;
        result.checkpoint = cp;
        result.report = report;
        return result;
    }

} // namespace agent_framework::live
