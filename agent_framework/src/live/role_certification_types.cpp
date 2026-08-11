#include "agent/live/role_certification.hpp"

#include <algorithm>
#include <set>

namespace agent_framework::live
{
    namespace
    {

        template <typename T>
        std::string enum_name(T value, std::initializer_list<std::pair<T, const char *>> names)
        {
            for (const auto &[candidate, name] : names)
                if (candidate == value)
                    return name;
            return "unknown";
        }

        std::optional<LiveCellOutcome> outcome_from(std::string_view value)
        {
            if (value == "passed")
                return LiveCellOutcome::Passed;
            if (value == "failed")
                return LiveCellOutcome::Failed;
            if (value == "inconclusive")
                return LiveCellOutcome::Inconclusive;
            return std::nullopt;
        }
        std::optional<RoleCertificationState> state_from(std::string_view value)
        {
            if (value == "running")
                return RoleCertificationState::Running;
            if (value == "inconclusive")
                return RoleCertificationState::Inconclusive;
            if (value == "awaiting_approval")
                return RoleCertificationState::AwaitingApproval;
            if (value == "certified")
                return RoleCertificationState::Certified;
            if (value == "rejected")
                return RoleCertificationState::Rejected;
            if (value == "failed")
                return RoleCertificationState::Failed;
            if (value == "cancelled")
                return RoleCertificationState::Cancelled;
            return std::nullopt;
        }
        std::optional<LiveCellKind> kind_from(std::string_view value)
        {
            if (value == "cognition")
                return LiveCellKind::Cognition;
            if (value == "memory")
                return LiveCellKind::Memory;
            if (value == "execution")
                return LiveCellKind::Execution;
            if (value == "assurance")
                return LiveCellKind::Assurance;
            if (value == "judge")
                return LiveCellKind::Judge;
            if (value == "failure_recovery")
                return LiveCellKind::FailureRecovery;
            return std::nullopt;
        }
        std::optional<LiveFailureScenario> failure_from(std::string_view value)
        {
            if (value == "none")
                return LiveFailureScenario::None;
            if (value == "timeout")
                return LiveFailureScenario::Timeout;
            if (value == "rate_limit")
                return LiveFailureScenario::RateLimit;
            if (value == "malformed_output")
                return LiveFailureScenario::MalformedOutput;
            if (value == "provider_fallback")
                return LiveFailureScenario::ProviderFallback;
            if (value == "restart")
                return LiveFailureScenario::Restart;
            if (value == "cost_cap")
                return LiveFailureScenario::CostCap;
            return std::nullopt;
        }
        bool exact_fields(const nlohmann::json &j, std::initializer_list<const char *> fields)
        {
            if (!j.is_object() || j.size() != fields.size())
                return false;
            return std::all_of(fields.begin(), fields.end(), [&](const char *field)
                               { return j.contains(field); });
        }

        nlohmann::json profile_json(const ProviderProfileManifest &v)
        {
            return {{"role", v.role}, {"stage", v.stage}, {"profile_id", v.profile_id}, {"profile_revision", v.profile_revision}, {"prompt_id", v.prompt_id}, {"prompt_revision", v.prompt_revision}, {"provider", v.provider}, {"model", v.model}, {"adapter_revision", v.adapter_revision}, {"model_family", v.model_family}, {"independence_group", v.independence_group}, {"region", v.region}, {"config_digest", v.config_digest}, {"calibration_id", v.calibration_id}, {"calibration_digest", v.calibration_digest}, {"calibration_approved", v.calibration_approved}, {"capabilities", v.capabilities}, {"dependency_digests", v.dependency_digests}};
        }

        nlohmann::json spec_json(const LiveCellSpec &v)
        {
            return {{"cell_id", v.cell_id}, {"kind", live_cell_kind_name(v.kind)}, {"role", v.role}, {"stage", v.stage}, {"required", v.required}, {"dependencies", v.dependencies}, {"profile_id", v.profile_id}, {"profile_revision", v.profile_revision}, {"prompt_id", v.prompt_id}, {"prompt_revision", v.prompt_revision}, {"provider", v.provider}, {"model", v.model}, {"independence_group", v.independence_group}, {"region", v.region}, {"required_capabilities", v.required_capabilities}, {"read_only", v.read_only}, {"blind", v.blind}, {"strong_oracle_required", v.strong_oracle_required}, {"failure_scenario", live_failure_scenario_name(v.failure_scenario)}, {"max_latency_ms", v.max_latency_ms}, {"max_tokens", v.max_tokens}, {"max_cost_usd", v.max_cost_usd}};
        }

        nlohmann::json result_json(const LiveCellResult &v)
        {
            return {{"cell_id", v.cell_id}, {"spec_digest", v.spec_digest}, {"executed", v.executed}, {"outcome", live_cell_outcome_name(v.outcome)}, {"reason", v.reason}, {"error_class", v.error_class}, {"invocation_id", v.invocation_id}, {"invocation_manifest_digest", v.invocation_manifest_digest}, {"role", v.role}, {"profile_id", v.profile_id}, {"profile_revision", v.profile_revision}, {"prompt_id", v.prompt_id}, {"prompt_revision", v.prompt_revision}, {"provider", v.provider}, {"model", v.model}, {"independence_group", v.independence_group}, {"region", v.region}, {"capabilities", v.capabilities}, {"evidence_digests", v.evidence_digests}, {"oracle_digests", v.oracle_digests}, {"read_only", v.read_only}, {"blind", v.blind}, {"recovered", v.recovered}, {"fallback_used", v.fallback_used}, {"unauthorized_memory_promotion", v.unauthorized_memory_promotion}, {"latency_ms", v.latency_ms}, {"tokens", v.tokens}, {"cost_usd", v.cost_usd}, {"started_at", v.started_at}, {"finished_at", v.finished_at}};
        }

        std::optional<LiveCellResult> result_from(const nlohmann::json &j)
        {
            if (!exact_fields(j, {"cell_id", "spec_digest", "executed", "outcome", "reason", "error_class", "invocation_id", "invocation_manifest_digest", "role", "profile_id", "profile_revision", "prompt_id", "prompt_revision", "provider", "model", "independence_group", "region", "capabilities", "evidence_digests", "oracle_digests", "read_only", "blind", "recovered", "fallback_used", "unauthorized_memory_promotion", "latency_ms", "tokens", "cost_usd", "started_at", "finished_at"}))
                return std::nullopt;
            try
            {
                LiveCellResult v;
                v.cell_id = j.at("cell_id");
                v.spec_digest = j.at("spec_digest");
                v.executed = j.at("executed");
                auto outcome = outcome_from(j.at("outcome").get<std::string>());
                if (!outcome)
                    return std::nullopt;
                v.outcome = *outcome;
                v.reason = j.at("reason");
                v.error_class = j.at("error_class");
                v.invocation_id = j.at("invocation_id");
                v.invocation_manifest_digest = j.at("invocation_manifest_digest");
                v.role = j.at("role");
                v.profile_id = j.at("profile_id");
                v.profile_revision = j.at("profile_revision");
                v.prompt_id = j.at("prompt_id");
                v.prompt_revision = j.at("prompt_revision");
                v.provider = j.at("provider");
                v.model = j.at("model");
                v.independence_group = j.at("independence_group");
                v.region = j.at("region");
                v.capabilities = j.at("capabilities").get<std::vector<std::string>>();
                v.evidence_digests = j.at("evidence_digests").get<std::vector<std::string>>();
                v.oracle_digests = j.at("oracle_digests").get<std::vector<std::string>>();
                v.read_only = j.at("read_only");
                v.blind = j.at("blind");
                v.recovered = j.at("recovered");
                v.fallback_used = j.at("fallback_used");
                v.unauthorized_memory_promotion = j.at("unauthorized_memory_promotion");
                v.latency_ms = j.at("latency_ms");
                v.tokens = j.at("tokens");
                v.cost_usd = j.at("cost_usd");
                v.started_at = j.at("started_at");
                v.finished_at = j.at("finished_at");
                return v;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        nlohmann::json signature_json(const SignatureEnvelope &v)
        {
            return {{"algorithm", v.algorithm}, {"key_id", v.key_id}, {"signed_digest", v.signed_digest}, {"signature", v.signature}};
        }
        std::optional<SignatureEnvelope> signature_from(const nlohmann::json &j)
        {
            if (!exact_fields(j, {"algorithm", "key_id", "signed_digest", "signature"}))
                return std::nullopt;
            try
            {
                return SignatureEnvelope{j.at("algorithm"), j.at("key_id"), j.at("signed_digest"), j.at("signature")};
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        std::optional<ProviderProfileManifest> profile_from(const nlohmann::json &j)
        {
            if (!exact_fields(j, {"role", "stage", "profile_id", "profile_revision", "prompt_id", "prompt_revision", "provider", "model", "adapter_revision", "model_family", "independence_group", "region", "config_digest", "calibration_id", "calibration_digest", "calibration_approved", "capabilities", "dependency_digests"}))
                return std::nullopt;
            try
            {
                ProviderProfileManifest v;
                v.role = j.at("role");
                v.stage = j.at("stage");
                v.profile_id = j.at("profile_id");
                v.profile_revision = j.at("profile_revision");
                v.prompt_id = j.at("prompt_id");
                v.prompt_revision = j.at("prompt_revision");
                v.provider = j.at("provider");
                v.model = j.at("model");
                v.adapter_revision = j.at("adapter_revision");
                v.model_family = j.at("model_family");
                v.independence_group = j.at("independence_group");
                v.region = j.at("region");
                v.config_digest = j.at("config_digest");
                v.calibration_id = j.at("calibration_id");
                v.calibration_digest = j.at("calibration_digest");
                v.calibration_approved = j.at("calibration_approved");
                v.capabilities = j.at("capabilities").get<std::vector<std::string>>();
                v.dependency_digests = j.at("dependency_digests").get<std::vector<std::string>>();
                return v;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }
        std::optional<LiveCellSpec> spec_from(const nlohmann::json &j)
        {
            if (!exact_fields(j, {"cell_id", "kind", "role", "stage", "required", "dependencies", "profile_id", "profile_revision", "prompt_id", "prompt_revision", "provider", "model", "independence_group", "region", "required_capabilities", "read_only", "blind", "strong_oracle_required", "failure_scenario", "max_latency_ms", "max_tokens", "max_cost_usd"}))
                return std::nullopt;
            try
            {
                LiveCellSpec v;
                v.cell_id = j.at("cell_id");
                auto kind = kind_from(j.at("kind").get<std::string>());
                auto failure = failure_from(j.at("failure_scenario").get<std::string>());
                if (!kind || !failure)
                    return std::nullopt;
                v.kind = *kind;
                v.failure_scenario = *failure;
                v.role = j.at("role");
                v.stage = j.at("stage");
                v.required = j.at("required");
                v.dependencies = j.at("dependencies").get<std::vector<std::string>>();
                v.profile_id = j.at("profile_id");
                v.profile_revision = j.at("profile_revision");
                v.prompt_id = j.at("prompt_id");
                v.prompt_revision = j.at("prompt_revision");
                v.provider = j.at("provider");
                v.model = j.at("model");
                v.independence_group = j.at("independence_group");
                v.region = j.at("region");
                v.required_capabilities = j.at("required_capabilities").get<std::vector<std::string>>();
                v.read_only = j.at("read_only");
                v.blind = j.at("blind");
                v.strong_oracle_required = j.at("strong_oracle_required");
                v.max_latency_ms = j.at("max_latency_ms");
                v.max_tokens = j.at("max_tokens");
                v.max_cost_usd = j.at("max_cost_usd");
                return v;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        nlohmann::json report_payload(const RoleCertificationReport &v, bool include_signature)
        {
            nlohmann::json cells = nlohmann::json::array();
            for (const auto &c : v.cells)
                cells.push_back(result_json(c));
            nlohmann::json p = {{"workflow_id", v.workflow_id}, {"executed", v.executed}, {"state", role_certification_state_name(v.state)}, {"environment_digest", v.environment_digest}, {"matrix_digest", v.matrix_digest}, {"cells", cells}, {"metrics", v.metrics}, {"findings", v.findings}, {"blockers", v.blockers}, {"residual_risks", v.residual_risks}, {"approval_decision_id", v.approval_decision_id}, {"issued_at", v.issued_at}, {"expires_at", v.expires_at}};
            if (include_signature)
                p["signature"] = signature_json(v.signature);
            return p;
        }

        bool has_all(const std::vector<std::string> &actual, const std::vector<std::string> &required)
        {
            return std::all_of(required.begin(), required.end(), [&](const auto &x)
                               { return std::find(actual.begin(), actual.end(), x) != actual.end(); });
        }
        void add(std::vector<std::string> &out, std::string value)
        {
            if (std::find(out.begin(), out.end(), value) == out.end())
                out.push_back(std::move(value));
        }
    }

    std::string live_cell_kind_name(LiveCellKind v) { return enum_name(v, {{LiveCellKind::Cognition, "cognition"}, {LiveCellKind::Memory, "memory"}, {LiveCellKind::Execution, "execution"}, {LiveCellKind::Assurance, "assurance"}, {LiveCellKind::Judge, "judge"}, {LiveCellKind::FailureRecovery, "failure_recovery"}}); }
    std::string live_cell_outcome_name(LiveCellOutcome v) { return enum_name(v, {{LiveCellOutcome::Passed, "passed"}, {LiveCellOutcome::Failed, "failed"}, {LiveCellOutcome::Inconclusive, "inconclusive"}}); }
    std::string live_failure_scenario_name(LiveFailureScenario v) { return enum_name(v, {{LiveFailureScenario::None, "none"}, {LiveFailureScenario::Timeout, "timeout"}, {LiveFailureScenario::RateLimit, "rate_limit"}, {LiveFailureScenario::MalformedOutput, "malformed_output"}, {LiveFailureScenario::ProviderFallback, "provider_fallback"}, {LiveFailureScenario::Restart, "restart"}, {LiveFailureScenario::CostCap, "cost_cap"}}); }
    std::string role_certification_state_name(RoleCertificationState v) { return enum_name(v, {{RoleCertificationState::Running, "running"}, {RoleCertificationState::Inconclusive, "inconclusive"}, {RoleCertificationState::AwaitingApproval, "awaiting_approval"}, {RoleCertificationState::Certified, "certified"}, {RoleCertificationState::Rejected, "rejected"}, {RoleCertificationState::Failed, "failed"}, {RoleCertificationState::Cancelled, "cancelled"}}); }

    nlohmann::json encode(const LiveEnvironmentProfile &v)
    {
        nlohmann::json profiles = nlohmann::json::array();
        for (const auto &p : v.role_profiles)
            profiles.push_back(profile_json(p));
        return contracts::make_typed_contract(v.metadata, "phase4.live_environment_profile", {{"certification_id", v.certification_id}, {"revision", v.revision}, {"os", v.os}, {"build_digest", v.build_digest}, {"git_revision", v.git_revision}, {"config_digest", v.config_digest}, {"provider_registry_digest", v.provider_registry_digest}, {"memory_policy_digest", v.memory_policy_digest}, {"sandbox_policy_digest", v.sandbox_policy_digest}, {"telemetry_policy_digest", v.telemetry_policy_digest}, {"evaluation_suite_digest", v.evaluation_suite_digest}, {"endpoint_class", v.endpoint_class}, {"region", v.region}, {"dependency_digests", v.dependency_digests}, {"secret_refs", v.secret_refs}, {"role_profiles", profiles}, {"scheduled_at", v.scheduled_at}, {"expires_at", v.expires_at}});
    }
    nlohmann::json encode(const RoleLiveMatrix &v)
    {
        nlohmann::json cells = nlohmann::json::array();
        for (const auto &c : v.cells)
            cells.push_back(spec_json(c));
        return contracts::make_typed_contract(v.metadata, "phase4.role_live_matrix", {{"matrix_id", v.matrix_id}, {"revision", v.revision}, {"environment_digest", v.environment_digest}, {"cells", cells}, {"minimum_production_combinations", v.minimum_production_combinations}});
    }
    nlohmann::json encode(const LiveCellResult &v) { return result_json(v); }
    nlohmann::json encode(const RoleCertificationReport &v) { return contracts::make_typed_contract(v.metadata, "phase4.role_certification_report", report_payload(v, true)); }
    nlohmann::json encode(const RoleCertificationCheckpoint &v)
    {
        nlohmann::json cells = nlohmann::json::array();
        for (const auto &c : v.cells)
            cells.push_back(result_json(c));
        return contracts::make_typed_contract(v.metadata, "phase4.role_certification_checkpoint", {{"workflow_id", v.workflow_id}, {"revision", v.revision}, {"state", role_certification_state_name(v.state)}, {"environment_digest", v.environment_digest}, {"matrix_digest", v.matrix_digest}, {"next_cell", v.next_cell}, {"cells", cells}, {"blockers", v.blockers}, {"report_digest", v.report_digest}, {"error_code", v.error_code}, {"error_message", v.error_message}, {"updated_at", v.updated_at}});
    }

    std::string role_environment_digest(const LiveEnvironmentProfile &v) { return encode(v).at("canonical_digest"); }
    std::string live_cell_spec_digest(const LiveCellSpec &v) { return contracts::embedded_digest(spec_json(v)).value_or(""); }
    std::string role_live_matrix_digest(const RoleLiveMatrix &v) { return encode(v).at("canonical_digest"); }
    std::string role_report_signing_digest(const RoleCertificationReport &v) { return contracts::embedded_digest(report_payload(v, false)).value_or(""); }

    std::optional<LiveEnvironmentProfile> decode_live_environment_profile(const nlohmann::json &j, const contracts::ParseContext &context, std::vector<contracts::ContractIssue> *issues)
    {
        auto d = contracts::parse_typed_contract(j, "phase4.live_environment_profile", context, issues);
        if (!d || !exact_fields(d->payload, {"certification_id", "revision", "os", "build_digest", "git_revision", "config_digest", "provider_registry_digest", "memory_policy_digest", "sandbox_policy_digest", "telemetry_policy_digest", "evaluation_suite_digest", "endpoint_class", "region", "dependency_digests", "secret_refs", "role_profiles", "scheduled_at", "expires_at"}))
            return std::nullopt;
        try
        {
            const auto &p = d->payload;
            LiveEnvironmentProfile v;
            v.metadata = d->metadata;
            v.certification_id = p.at("certification_id");
            v.revision = p.at("revision");
            v.os = p.at("os");
            v.build_digest = p.at("build_digest");
            v.git_revision = p.at("git_revision");
            v.config_digest = p.at("config_digest");
            v.provider_registry_digest = p.at("provider_registry_digest");
            v.memory_policy_digest = p.at("memory_policy_digest");
            v.sandbox_policy_digest = p.at("sandbox_policy_digest");
            v.telemetry_policy_digest = p.at("telemetry_policy_digest");
            v.evaluation_suite_digest = p.at("evaluation_suite_digest");
            v.endpoint_class = p.at("endpoint_class");
            v.region = p.at("region");
            v.dependency_digests = p.at("dependency_digests").get<std::vector<std::string>>();
            v.secret_refs = p.at("secret_refs").get<std::vector<std::string>>();
            for (const auto &x : p.at("role_profiles"))
            {
                auto profile = profile_from(x);
                if (!profile)
                    return std::nullopt;
                v.role_profiles.push_back(std::move(*profile));
            }
            v.scheduled_at = p.at("scheduled_at");
            v.expires_at = p.at("expires_at");
            return v;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
    std::optional<RoleLiveMatrix> decode_role_live_matrix(const nlohmann::json &j, const contracts::ParseContext &context, std::vector<contracts::ContractIssue> *issues)
    {
        auto d = contracts::parse_typed_contract(j, "phase4.role_live_matrix", context, issues);
        if (!d || !exact_fields(d->payload, {"matrix_id", "revision", "environment_digest", "cells", "minimum_production_combinations"}))
            return std::nullopt;
        try
        {
            const auto &p = d->payload;
            RoleLiveMatrix v;
            v.metadata = d->metadata;
            v.matrix_id = p.at("matrix_id");
            v.revision = p.at("revision");
            v.environment_digest = p.at("environment_digest");
            v.minimum_production_combinations = p.at("minimum_production_combinations");
            for (const auto &x : p.at("cells"))
            {
                auto cell = spec_from(x);
                if (!cell)
                    return std::nullopt;
                v.cells.push_back(std::move(*cell));
            }
            return v;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<RoleCertificationReport> decode_role_certification_report(const nlohmann::json &j, const contracts::ParseContext &context, std::vector<contracts::ContractIssue> *issues)
    {
        auto d = contracts::parse_typed_contract(j, "phase4.role_certification_report", context, issues);
        if (!d || !exact_fields(d->payload, {"workflow_id", "executed", "state", "environment_digest", "matrix_digest", "cells", "metrics", "findings", "blockers", "residual_risks", "approval_decision_id", "issued_at", "expires_at", "signature"}))
            return std::nullopt;
        try
        {
            const auto &p = d->payload;
            RoleCertificationReport v;
            v.metadata = d->metadata;
            v.workflow_id = p.at("workflow_id");
            v.executed = p.at("executed");
            auto state = state_from(p.at("state").get<std::string>());
            if (!state)
                return std::nullopt;
            v.state = *state;
            v.environment_digest = p.at("environment_digest");
            v.matrix_digest = p.at("matrix_digest");
            for (const auto &x : p.at("cells"))
            {
                auto c = result_from(x);
                if (!c)
                    return std::nullopt;
                v.cells.push_back(std::move(*c));
            }
            v.metrics = p.at("metrics").get<std::map<std::string, double>>();
            v.findings = p.at("findings").get<std::vector<std::string>>();
            v.blockers = p.at("blockers").get<std::vector<std::string>>();
            v.residual_risks = p.at("residual_risks").get<std::vector<std::string>>();
            v.approval_decision_id = p.at("approval_decision_id");
            v.issued_at = p.at("issued_at");
            v.expires_at = p.at("expires_at");
            auto s = signature_from(p.at("signature"));
            if (!s)
                return std::nullopt;
            v.signature = *s;
            return v;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
    std::optional<RoleCertificationCheckpoint> decode_role_certification_checkpoint(const nlohmann::json &j, const contracts::ParseContext &context, std::vector<contracts::ContractIssue> *issues)
    {
        auto d = contracts::parse_typed_contract(j, "phase4.role_certification_checkpoint", context, issues);
        if (!d || !exact_fields(d->payload, {"workflow_id", "revision", "state", "environment_digest", "matrix_digest", "next_cell", "cells", "blockers", "report_digest", "error_code", "error_message", "updated_at"}))
            return std::nullopt;
        try
        {
            const auto &p = d->payload;
            RoleCertificationCheckpoint v;
            v.metadata = d->metadata;
            v.workflow_id = p.at("workflow_id");
            v.revision = p.at("revision");
            auto state = state_from(p.at("state").get<std::string>());
            if (!state)
                return std::nullopt;
            v.state = *state;
            v.environment_digest = p.at("environment_digest");
            v.matrix_digest = p.at("matrix_digest");
            v.next_cell = p.at("next_cell");
            for (const auto &x : p.at("cells"))
            {
                auto c = result_from(x);
                if (!c)
                    return std::nullopt;
                v.cells.push_back(std::move(*c));
            }
            v.blockers = p.at("blockers").get<std::vector<std::string>>();
            v.report_digest = p.at("report_digest");
            v.error_code = p.at("error_code");
            v.error_message = p.at("error_message");
            v.updated_at = p.at("updated_at");
            return v;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::vector<std::string> validate_live_contract(const LiveEnvironmentProfile &e, const RoleLiveMatrix &m)
    {
        std::vector<std::string> out;
        if (e.metadata.identity.tenant_id.empty() || e.metadata.identity.task_id.empty() || e.certification_id.empty())
            add(out, "environment_identity_missing");
        if (e.revision == 0 || e.build_digest.empty() || e.git_revision.empty() || e.config_digest.empty() || e.provider_registry_digest.empty() || e.memory_policy_digest.empty() || e.sandbox_policy_digest.empty() || e.telemetry_policy_digest.empty() || e.evaluation_suite_digest.empty())
            add(out, "environment_revision_or_digest_missing");
        if (e.endpoint_class != "production")
            add(out, "endpoint_not_production");
        if (e.region.empty() || e.scheduled_at.empty() || e.expires_at.empty())
            add(out, "environment_schedule_or_region_missing");
        if (e.secret_refs.empty())
            add(out, "secret_references_missing");
        if (m.metadata.identity.tenant_id != e.metadata.identity.tenant_id || m.metadata.identity.task_id != e.metadata.identity.task_id || m.environment_digest != role_environment_digest(e))
            add(out, "matrix_environment_binding_invalid");
        if (m.matrix_id.empty() || m.revision == 0 || m.cells.empty() || m.minimum_production_combinations == 0)
            add(out, "matrix_identity_or_cells_invalid");
        std::set<std::string> ids;
        std::set<LiveCellKind> kinds;
        std::set<std::string> profiles;
        for (const auto &p : e.role_profiles)
        {
            profiles.insert(p.role + '\n' + p.stage);
            if (p.profile_id.empty() || p.profile_revision.empty() || p.prompt_id.empty() || p.prompt_revision.empty() || p.provider.empty() || p.model.empty() || p.adapter_revision.empty() || p.independence_group.empty() || p.region.empty() || p.config_digest.empty() || p.calibration_id.empty() || p.calibration_digest.empty() || !p.calibration_approved)
                add(out, "role_profile_invalid:" + p.role + ":" + p.stage);
        }
        for (const auto &c : m.cells)
        {
            if (c.cell_id.empty() || !ids.insert(c.cell_id).second)
                add(out, "cell_id_invalid_or_duplicate:" + c.cell_id);
            kinds.insert(c.kind);
            if (c.role.empty() || c.stage.empty() || c.profile_id.empty() || c.profile_revision.empty() || c.prompt_id.empty() || c.prompt_revision.empty() || c.provider.empty() || c.model.empty() || c.independence_group.empty() || c.region.empty())
                add(out, "cell_binding_missing:" + c.cell_id);
            if (!profiles.count(c.role + '\n' + c.stage))
                add(out, "cell_profile_manifest_missing:" + c.cell_id);
            if ((c.kind == LiveCellKind::Assurance || c.kind == LiveCellKind::Judge) && (!c.read_only || !c.blind))
                add(out, "assurance_or_judge_not_readonly_blind:" + c.cell_id);
            if (c.kind == LiveCellKind::Assurance && !c.strong_oracle_required)
                add(out, "assurance_strong_oracle_not_required:" + c.cell_id);
        }
        for (auto kind : {LiveCellKind::Cognition, LiveCellKind::Memory, LiveCellKind::Execution, LiveCellKind::Assurance, LiveCellKind::Judge})
            if (!kinds.count(kind))
                add(out, "required_pipeline_kind_missing:" + live_cell_kind_name(kind));
        for (const auto &c : m.cells)
            for (const auto &d : c.dependencies)
                if (!ids.count(d))
                    add(out, "cell_dependency_missing:" + c.cell_id + ":" + d);
        return out;
    }

    std::vector<std::string> validate_live_results(const LiveEnvironmentProfile &e, const RoleLiveMatrix &m, const std::vector<LiveCellResult> &results)
    {
        auto out = validate_live_contract(e, m);
        std::map<std::string, const LiveCellResult *> by;
        for (const auto &r : results)
            by[r.cell_id] = &r;
        std::map<std::string, const ProviderProfileManifest *> profiles;
        for (const auto &p : e.role_profiles)
            profiles[p.role + '\n' + p.stage] = &p;
        for (const auto &c : m.cells)
        {
            auto it = by.find(c.cell_id);
            if (it == by.end())
            {
                if (c.required)
                    add(out, "required_not_executed:" + c.cell_id);
                continue;
            }
            const auto &r = *it->second;
            if (r.spec_digest != live_cell_spec_digest(c))
                add(out, "cell_spec_digest_mismatch:" + c.cell_id);
            if (c.required && !r.executed)
                add(out, "required_not_executed:" + c.cell_id);
            if (c.required && r.outcome != LiveCellOutcome::Passed)
                add(out, "required_not_passed:" + c.cell_id);
            if (r.executed && (r.invocation_id.empty() || r.invocation_manifest_digest.empty() || r.evidence_digests.empty()))
                add(out, "execution_evidence_missing:" + c.cell_id);
            if (r.role != c.role || r.profile_id != c.profile_id || r.profile_revision != c.profile_revision || r.prompt_id != c.prompt_id || r.prompt_revision != c.prompt_revision || r.provider != c.provider || r.model != c.model || r.independence_group != c.independence_group || r.region != c.region)
                add(out, "actual_role_identity_mismatch:" + c.cell_id);
            if (!has_all(r.capabilities, c.required_capabilities))
                add(out, "required_capability_missing:" + c.cell_id);
            if (c.read_only && !r.read_only)
                add(out, "read_only_attestation_missing:" + c.cell_id);
            if (c.blind && !r.blind)
                add(out, "blind_attestation_missing:" + c.cell_id);
            if (c.strong_oracle_required && r.oracle_digests.empty())
                add(out, "strong_oracle_missing:" + c.cell_id);
            if (c.kind == LiveCellKind::Memory && r.unauthorized_memory_promotion)
                add(out, "unauthorized_memory_promotion:" + c.cell_id);
            if (c.max_latency_ms && r.latency_ms > c.max_latency_ms)
                add(out, "latency_budget_exceeded:" + c.cell_id);
            if (c.max_tokens && r.tokens > c.max_tokens)
                add(out, "token_budget_exceeded:" + c.cell_id);
            if (c.max_cost_usd > 0 && r.cost_usd > c.max_cost_usd)
                add(out, "cost_budget_exceeded:" + c.cell_id);
            for (const auto &d : c.dependencies)
            {
                auto dep = by.find(d);
                if (dep == by.end() || dep->second->outcome != LiveCellOutcome::Passed)
                    add(out, "dependency_not_passed:" + c.cell_id + ":" + d);
            }
            if (c.failure_scenario != LiveFailureScenario::None)
            {
                if (!r.recovered)
                    add(out, "failure_not_recovered:" + c.cell_id);
                if (c.failure_scenario == LiveFailureScenario::ProviderFallback && !r.fallback_used)
                    add(out, "fallback_not_used:" + c.cell_id);
            }
            auto p = profiles.find(c.role + '\n' + c.stage);
            if (p != profiles.end())
            {
                const auto &x = *p->second;
                if (x.profile_id != r.profile_id || x.profile_revision != r.profile_revision || x.prompt_id != r.prompt_id || x.prompt_revision != r.prompt_revision || x.provider != r.provider || x.model != r.model || x.independence_group != r.independence_group || x.region != r.region || !has_all(r.capabilities, x.capabilities))
                    add(out, "environment_role_manifest_mismatch:" + c.cell_id);
            }
        }
        for (const auto &a : m.cells)
            for (const auto &b : m.cells)
                if (a.cell_id < b.cell_id)
                {
                    const bool critic_pair = (a.kind == LiveCellKind::Cognition && a.stage.find("critic") != std::string::npos) || (b.kind == LiveCellKind::Cognition && b.stage.find("critic") != std::string::npos);
                    const bool execution_review_pair = (a.kind == LiveCellKind::Execution && (b.kind == LiveCellKind::Assurance || b.kind == LiveCellKind::Judge)) || (b.kind == LiveCellKind::Execution && (a.kind == LiveCellKind::Assurance || a.kind == LiveCellKind::Judge));
                    const bool dual_review_pair = (a.kind == LiveCellKind::Assurance && b.kind == LiveCellKind::Judge) || (b.kind == LiveCellKind::Assurance && a.kind == LiveCellKind::Judge);
                    if ((critic_pair || execution_review_pair || dual_review_pair) && a.independence_group == b.independence_group)
                        add(out, "independence_group_collision:" + a.cell_id + ":" + b.cell_id);
                    if ((execution_review_pair || dual_review_pair) && a.provider == b.provider && a.model == b.model)
                        add(out, "reviewer_provider_model_not_independent:" + a.cell_id + ":" + b.cell_id);
                }
        return out;
    }

    bool role_certification_valid_for(const RoleCertificationReport &r, const LiveEnvironmentProfile &e, const RoleLiveMatrix &m, std::string_view now, const std::function<bool(const SignatureEnvelope &)> &verifier, const std::function<bool(const LiveEnvironmentProfile &, const LiveCellSpec &, const LiveCellResult &, std::string *)> &cell_evidence_verifier)
    {
        if (r.state != RoleCertificationState::Certified || !r.executed || r.environment_digest != role_environment_digest(e) || r.matrix_digest != role_live_matrix_digest(m) || r.expires_at.empty() || (!now.empty() && r.expires_at < now) || r.signature.signed_digest != role_report_signing_digest(r) || !verifier || !verifier(r.signature))
            return false;
        if (!validate_live_results(e, m, r.cells).empty()) return false;
        if (e.endpoint_class == "production")
        {
            if (!cell_evidence_verifier) return false;
            for (const auto &spec : m.cells)
            {
                const auto found = std::find_if(r.cells.begin(), r.cells.end(),
                    [&](const auto &cell) { return cell.cell_id == spec.cell_id; });
                if (found == r.cells.end() || !found->executed ||
                    !cell_evidence_verifier(e, spec, *found, nullptr)) return false;
            }
        }
        return true;
    }

} // namespace agent_framework::live
