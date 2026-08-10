#pragma once

#include <map>

#include "agent/live/role_certification.hpp"

namespace phase4_live_test
{
    using namespace agent_framework;
    using namespace agent_framework::live;

    inline contracts::ContractMetadata metadata(std::string task = "task-f7l")
    {
        contracts::ContractMetadata m;
        m.identity.tenant_id = "tenant-live";
        m.identity.organization_id = "org-live";
        m.identity.project_id = "project-live";
        m.identity.task_id = std::move(task);
        m.identity.run_id = "run-live";
        return m;
    }
    inline LiveEnvironmentProfile environment(std::string task = "task-f7l")
    {
        LiveEnvironmentProfile e;
        e.metadata = metadata(task);
        e.certification_id = "cert-f7l";
        e.os = "linux-x86_64";
        e.build_digest = "sha256:build";
        e.git_revision = "git:abc123";
        e.config_digest = "sha256:config";
        e.provider_registry_digest = "sha256:providers";
        e.memory_policy_digest = "sha256:memory";
        e.sandbox_policy_digest = "sha256:sandbox";
        e.telemetry_policy_digest = "sha256:otel";
        e.evaluation_suite_digest = "sha256:suite";
        e.endpoint_class = "production";
        e.region = "ap-southeast-1";
        e.dependency_digests = {"sha256:dependency"};
        e.secret_refs = {"kms://taskflow/live/provider"};
        e.scheduled_at = "2026-08-10T00:00:00Z";
        e.expires_at = "2026-08-17T00:00:00Z";
        return e;
    }
    inline LiveCellSpec cell(std::string id, LiveCellKind kind, std::string role, std::string stage, std::string group, std::vector<std::string> dependencies = {})
    {
        LiveCellSpec c;
        c.cell_id = std::move(id);
        c.kind = kind;
        c.role = std::move(role);
        c.stage = std::move(stage);
        c.dependencies = std::move(dependencies);
        c.profile_id = "profile-" + c.cell_id;
        c.profile_revision = "r1";
        c.prompt_id = "prompt-" + c.cell_id;
        c.prompt_revision = "p1";
        c.provider = "provider-" + c.cell_id;
        c.model = "model-" + c.cell_id;
        c.independence_group = std::move(group);
        c.region = "ap-southeast-1";
        c.required_capabilities = {"structured-output"};
        c.max_latency_ms = 10000;
        c.max_tokens = 10000;
        c.max_cost_usd = 1.0;
        if (kind == LiveCellKind::Assurance || kind == LiveCellKind::Judge)
        {
            c.read_only = true;
            c.blind = true;
        }
        if (kind == LiveCellKind::Assurance)
            c.strong_oracle_required = true;
        return c;
    }
    inline ProviderProfileManifest profile(const LiveCellSpec &c)
    {
        ProviderProfileManifest p;
        p.role = c.role;
        p.stage = c.stage;
        p.profile_id = c.profile_id;
        p.profile_revision = c.profile_revision;
        p.prompt_id = c.prompt_id;
        p.prompt_revision = c.prompt_revision;
        p.provider = c.provider;
        p.model = c.model;
        p.adapter_revision = "adapter-r1";
        p.model_family = "family-" + c.cell_id;
        p.independence_group = c.independence_group;
        p.region = c.region;
        p.config_digest = "sha256:profile-" + c.cell_id;
        p.calibration_id = "cal-" + c.cell_id;
        p.calibration_digest = "sha256:cal-" + c.cell_id;
        p.calibration_approved = true;
        p.capabilities = c.required_capabilities;
        p.dependency_digests = {"sha256:adapter"};
        return p;
    }
    inline RoleLiveMatrix matrix(LiveEnvironmentProfile &e, bool failures = true)
    {
        RoleLiveMatrix m;
        m.metadata = e.metadata;
        m.matrix_id = "matrix-f7l";
        m.cells.push_back(cell("cognition-primary", LiveCellKind::Cognition, "planner", "primary", "grp-plan"));
        m.cells.push_back(cell("cognition-critic", LiveCellKind::Cognition, "critic", "critic", "grp-critic", {"cognition-primary"}));
        m.cells.push_back(cell("memory", LiveCellKind::Memory, "memory-curator", "consolidation", "grp-memory", {"cognition-critic"}));
        m.cells.push_back(cell("execution", LiveCellKind::Execution, "executor", "tool-execution", "grp-executor", {"memory"}));
        m.cells.push_back(cell("assurance", LiveCellKind::Assurance, "verifier", "professional-verification", "grp-verifier", {"execution"}));
        m.cells.push_back(cell("judge", LiveCellKind::Judge, "judge", "blind-evaluation", "grp-judge", {"assurance"}));
        if (failures)
        {
            for (const auto &x : std::vector<std::pair<std::string, LiveFailureScenario>>{{"timeout", LiveFailureScenario::Timeout}, {"rate-limit", LiveFailureScenario::RateLimit}, {"malformed", LiveFailureScenario::MalformedOutput}, {"fallback", LiveFailureScenario::ProviderFallback}, {"restart", LiveFailureScenario::Restart}, {"cost-cap", LiveFailureScenario::CostCap}})
            {
                auto c = cell("recovery-" + x.first, LiveCellKind::FailureRecovery, "recovery", "recovery-" + x.first, "grp-recovery-" + x.first, {"judge"});
                c.failure_scenario = x.second;
                m.cells.push_back(std::move(c));
            }
        }
        for (const auto &c : m.cells)
            e.role_profiles.push_back(profile(c));
        m.environment_digest = role_environment_digest(e);
        return m;
    }

    class DeterministicExecutor final : public LiveCellExecutor
    {
    public:
        std::map<std::string, std::function<void(LiveCellResult &)>> mutations;
        LiveCellResult execute(const LiveCellExecutionRequest &r) override
        {
            LiveCellResult x;
            x.cell_id = r.cell.cell_id;
            x.spec_digest = live_cell_spec_digest(r.cell);
            x.executed = true;
            x.outcome = LiveCellOutcome::Passed;
            x.invocation_id = "inv-" + r.cell.cell_id;
            x.invocation_manifest_digest = "sha256:manifest-" + r.cell.cell_id;
            x.role = r.cell.role;
            x.profile_id = r.cell.profile_id;
            x.profile_revision = r.cell.profile_revision;
            x.prompt_id = r.cell.prompt_id;
            x.prompt_revision = r.cell.prompt_revision;
            x.provider = r.cell.provider;
            x.model = r.cell.model;
            x.independence_group = r.cell.independence_group;
            x.region = r.cell.region;
            x.capabilities = r.cell.required_capabilities;
            x.evidence_digests = {"sha256:evidence-" + r.cell.cell_id};
            x.read_only = r.cell.read_only;
            x.blind = r.cell.blind;
            x.latency_ms = 100;
            x.tokens = 100;
            x.cost_usd = 0.01;
            x.started_at = "2026-08-10T01:00:00Z";
            x.finished_at = "2026-08-10T01:00:01Z";
            if (r.cell.strong_oracle_required)
                x.oracle_digests = {"sha256:oracle"};
            if (r.cell.failure_scenario != LiveFailureScenario::None)
                x.recovered = true;
            if (r.cell.failure_scenario == LiveFailureScenario::ProviderFallback)
                x.fallback_used = true;
            auto it = mutations.find(r.cell.cell_id);
            if (it != mutations.end())
                it->second(x);
            return x;
        }
    };
    inline SignatureEnvelope sign(std::string_view d) { return {"kms-test-v1", "kms://taskflow/live/signing", std::string(d), "signature:" + std::string(d)}; }
    inline bool verify(const SignatureEnvelope &s) { return s.algorithm == "kms-test-v1" && s.key_id == "kms://taskflow/live/signing" && s.signature == "signature:" + s.signed_digest; }
    inline RoleCertificationOptions options(std::string workflow = "workflow-f7l")
    {
        RoleCertificationOptions o;
        o.workflow_id = std::move(workflow);
        o.now = "2026-08-10T02:00:00Z";
        o.approval_decision_id = "approval-f7l";
        o.approval_validator = [](std::string_view d, std::string_view id)
        { return !d.empty() && id == "approval-f7l"; };
        o.signer = sign;
        o.signature_verifier = verify;
        return o;
    }
}
