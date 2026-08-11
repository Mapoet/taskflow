#include <agent/ui/phase4_operations.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace agent_framework
{
    namespace
    {

        constexpr std::size_t kMaxItems = 256;
        constexpr std::size_t kMaxText = 4096;

        std::string bounded(const json &object, const char *key, bool required = false)
        {
            if (!object.contains(key))
            {
                if (required)
                    throw std::invalid_argument(std::string("missing field: ") + key);
                return {};
            }
            if (!object[key].is_string())
                throw std::invalid_argument(std::string("field is not string: ") + key);
            std::string value = object[key].get<std::string>();
            if (value.size() > kMaxText)
                throw std::invalid_argument(std::string("field too large: ") + key);
            return value;
        }

        std::vector<std::string> strings(const json &object, const char *key)
        {
            std::vector<std::string> out;
            if (!object.contains(key))
                return out;
            if (!object[key].is_array() || object[key].size() > kMaxItems)
                throw std::invalid_argument(std::string("invalid string array: ") + key);
            for (const auto &item : object[key])
            {
                if (!item.is_string())
                    throw std::invalid_argument(std::string("invalid string array item: ") + key);
                auto value = item.get<std::string>();
                if (value.size() > kMaxText)
                    throw std::invalid_argument(std::string("array item too large: ") + key);
                out.push_back(std::move(value));
            }
            return out;
        }

        template <typename T, typename Parse>
        std::vector<T> objects(const json &root, const char *key, Parse parse)
        {
            std::vector<T> out;
            if (!root.contains(key))
                return out;
            if (!root[key].is_array() || root[key].size() > kMaxItems)
                throw std::invalid_argument(std::string("invalid object array: ") + key);
            out.reserve(root[key].size());
            for (const auto &value : root[key])
            {
                if (!value.is_object())
                    throw std::invalid_argument(std::string("invalid object item: ") + key);
                out.push_back(parse(value));
            }
            return out;
        }

        template <typename Values>
        void require_unique_ids(const Values &values, const char *field)
        {
            std::unordered_set<std::string> ids;
            for (const auto &value : values)
            {
                if (value.id.empty())
                    throw std::invalid_argument(std::string("empty id in ") + field);
                if (!ids.insert(value.id).second)
                    throw std::invalid_argument(std::string("duplicate id in ") + field + ": " + value.id);
            }
        }

        json status_json(OperationsStatus status) { return Phase4OperationsProjection::status_name(status); }

        std::string clipped(std::string value, std::size_t width)
        {
            if (value.size() <= width)
                return value;
            if (width < 4)
                return value.substr(0, width);
            return value.substr(0, width - 3) + "...";
        }

    } // namespace

    const char *Phase4OperationsProjection::status_name(OperationsStatus status) noexcept
    {
        switch (status)
        {
        case OperationsStatus::Unknown:
            return "unknown";
        case OperationsStatus::Pending:
            return "pending";
        case OperationsStatus::Running:
            return "running";
        case OperationsStatus::Passed:
            return "passed";
        case OperationsStatus::Warning:
            return "warning";
        case OperationsStatus::Blocked:
            return "blocked";
        case OperationsStatus::Failed:
            return "failed";
        }
        return "unknown";
    }

    OperationsStatus Phase4OperationsProjection::parse_status(std::string_view value)
    {
        if (value == "unknown")
            return OperationsStatus::Unknown;
        if (value == "pending")
            return OperationsStatus::Pending;
        if (value == "running")
            return OperationsStatus::Running;
        if (value == "passed")
            return OperationsStatus::Passed;
        if (value == "warning")
            return OperationsStatus::Warning;
        if (value == "blocked")
            return OperationsStatus::Blocked;
        if (value == "failed")
            return OperationsStatus::Failed;
        throw std::invalid_argument("invalid operations status: " + std::string(value));
    }

    json Phase4OperationsProjection::to_json(const Phase4OperationsSnapshot &s)
    {
        json out{{"schema_version", s.schema_version}, {"snapshot_id", s.snapshot_id}, {"tenant_id", s.tenant_id}, {"run_id", s.run_id}, {"task_id", s.task_id}, {"updated_at", s.updated_at}, {"overall_status", status_json(s.overall_status)}, {"plan_revision", s.plan_revision}, {"summary", s.summary}, {"blocker", s.blocker}, {"residual_risk", s.residual_risk}, {"live_certification", s.live_certification}, {"unknowns", s.unknowns}};
        out["stages"] = json::array();
        for (const auto &v : s.stages)
            out["stages"].push_back(json{{"id", v.id}, {"label", v.label}, {"status", status_json(v.status)}, {"revision", v.revision}, {"role", v.role}, {"summary", v.summary}, {"evidence_ids", v.evidence_ids}});
        out["evidence"] = json::array();
        for (const auto &v : s.evidence)
            out["evidence"].push_back(json{{"id", v.id}, {"claim", v.claim}, {"kind", v.kind}, {"source", v.source}, {"authority", v.authority}, {"freshness", v.freshness}, {"status", status_json(v.status)}, {"digest", v.digest}});
        out["memory"] = json::array();
        for (const auto &v : s.memory)
            out["memory"].push_back(json{{"id", v.id}, {"scope", v.scope}, {"source", v.source}, {"authority", v.authority}, {"freshness", v.freshness}, {"conflict", v.conflict}, {"selected", v.selected}, {"selection_reason", v.selection_reason}});
        out["invocations"] = json::array();
        for (const auto &v : s.invocations)
            out["invocations"].push_back(json{{"id", v.id}, {"role", v.role}, {"provider", v.provider}, {"model", v.model}, {"prompt_version", v.prompt_version}, {"view_id", v.view_id}, {"fallback", v.fallback}, {"input_tokens", v.input_tokens}, {"output_tokens", v.output_tokens}, {"cost_usd", v.cost_usd}, {"latency_ms", v.latency_ms}, {"status", status_json(v.status)}});
        out["assurance"] = json::array();
        for (const auto &v : s.assurance)
            out["assurance"].push_back(json{{"id", v.id}, {"label", v.label}, {"status", status_json(v.status)}, {"oracle", v.oracle}, {"verifier", v.verifier}, {"finding_ids", v.finding_ids}});
        out["hitl"] = json::array();
        for (const auto &v : s.hitl)
            out["hitl"].push_back(json{{"id", v.id}, {"kind", v.kind}, {"status", status_json(v.status)}, {"summary", v.summary}, {"requested_by", v.requested_by}, {"deadline", v.deadline}, {"allowed_actions", v.allowed_actions}});
        out["source_revisions"] = json::array();
        for (const auto &v : s.source_revisions)
            out["source_revisions"].push_back(json{{"store", v.store}, {"object_id", v.object_id},
                {"revision", v.revision}, {"digest", v.digest}});
        return out;
    }

    Phase4OperationsSnapshot Phase4OperationsProjection::from_json(const json &root)
    {
        if (!root.is_object())
            throw std::invalid_argument("operations snapshot must be an object");
        Phase4OperationsSnapshot s;
        s.schema_version = bounded(root, "schema_version", true);
        if (s.schema_version != "phase4.operations.v1")
            throw std::invalid_argument("unsupported operations schema");
        s.snapshot_id = bounded(root, "snapshot_id", true);
        s.tenant_id = bounded(root, "tenant_id", true);
        s.run_id = bounded(root, "run_id", true);
        s.task_id = bounded(root, "task_id", true);
        s.updated_at = bounded(root, "updated_at", true);
        if (s.snapshot_id.empty() || s.tenant_id.empty() || s.run_id.empty() || s.task_id.empty() || s.updated_at.empty())
            throw std::invalid_argument("operations identity fields must not be empty");
        s.overall_status = parse_status(bounded(root, "overall_status", true));
        s.plan_revision = root.value("plan_revision", std::uint64_t{0});
        s.summary = bounded(root, "summary");
        s.blocker = bounded(root, "blocker");
        s.residual_risk = bounded(root, "residual_risk");
        s.live_certification = bounded(root, "live_certification");
        s.unknowns = strings(root, "unknowns");
        s.stages = objects<OperationsStage>(root, "stages", [](const json &v)
                                            {
        OperationsStage x; x.id = bounded(v, "id", true); x.label = bounded(v, "label", true);
        x.status = Phase4OperationsProjection::parse_status(bounded(v, "status", true));
        x.revision = v.value("revision", std::uint64_t{0}); x.role = bounded(v, "role");
        x.summary = bounded(v, "summary"); x.evidence_ids = strings(v, "evidence_ids"); return x; });
        s.evidence = objects<OperationsEvidence>(root, "evidence", [](const json &v)
                                                 {
        OperationsEvidence x; x.id = bounded(v, "id", true); x.claim = bounded(v, "claim", true);
        x.kind = bounded(v, "kind"); x.source = bounded(v, "source"); x.authority = bounded(v, "authority");
        x.freshness = bounded(v, "freshness"); x.status = Phase4OperationsProjection::parse_status(bounded(v, "status", true));
        x.digest = bounded(v, "digest"); return x; });
        s.memory = objects<OperationsMemoryItem>(root, "memory", [](const json &v)
                                                 {
        OperationsMemoryItem x; x.id = bounded(v, "id", true); x.scope = bounded(v, "scope", true);
        x.source = bounded(v, "source"); x.authority = bounded(v, "authority"); x.freshness = bounded(v, "freshness");
        x.conflict = bounded(v, "conflict"); x.selected = v.value("selected", false); x.selection_reason = bounded(v, "selection_reason"); return x; });
        s.invocations = objects<OperationsInvocation>(root, "invocations", [](const json &v)
                                                      {
        OperationsInvocation x; x.id = bounded(v, "id", true); x.role = bounded(v, "role", true);
        x.provider = bounded(v, "provider"); x.model = bounded(v, "model"); x.prompt_version = bounded(v, "prompt_version");
        x.view_id = bounded(v, "view_id"); x.fallback = bounded(v, "fallback"); x.input_tokens = v.value("input_tokens", std::uint64_t{0});
        x.output_tokens = v.value("output_tokens", std::uint64_t{0}); x.cost_usd = v.value("cost_usd", 0.0);
        x.latency_ms = v.value("latency_ms", std::int64_t{0}); x.status = Phase4OperationsProjection::parse_status(bounded(v, "status", true));
        if (!std::isfinite(x.cost_usd) || x.cost_usd < 0.0 || x.latency_ms < 0) throw std::invalid_argument("invalid invocation metrics");
        return x; });
        s.assurance = objects<OperationsAssuranceLayer>(root, "assurance", [](const json &v)
                                                        {
        OperationsAssuranceLayer x; x.id = bounded(v, "id", true); x.label = bounded(v, "label", true);
        x.status = Phase4OperationsProjection::parse_status(bounded(v, "status", true)); x.oracle = bounded(v, "oracle");
        x.verifier = bounded(v, "verifier"); x.finding_ids = strings(v, "finding_ids"); return x; });
        s.hitl = objects<OperationsHitlRequest>(root, "hitl", [](const json &v)
                                                {
        OperationsHitlRequest x; x.id = bounded(v, "id", true); x.kind = bounded(v, "kind", true);
        x.status = Phase4OperationsProjection::parse_status(bounded(v, "status", true)); x.summary = bounded(v, "summary", true);
        x.requested_by = bounded(v, "requested_by"); x.deadline = bounded(v, "deadline"); x.allowed_actions = strings(v, "allowed_actions"); return x; });
        s.source_revisions = objects<OperationsSourceRevision>(root, "source_revisions", [](const json &v)
        {
        OperationsSourceRevision x; x.store = bounded(v, "store", true);
        x.object_id = bounded(v, "object_id", true); x.revision = v.value("revision", std::uint64_t{0});
        x.digest = bounded(v, "digest", true); return x; });
        require_unique_ids(s.stages, "stages");
        require_unique_ids(s.evidence, "evidence");
        require_unique_ids(s.memory, "memory");
        require_unique_ids(s.invocations, "invocations");
        require_unique_ids(s.assurance, "assurance");
        require_unique_ids(s.hitl, "hitl");
        std::unordered_set<std::string> evidence_ids;
        for (const auto &item : s.evidence)
            evidence_ids.insert(item.id);
        for (const auto &stage : s.stages)
            for (const auto &evidence_id : stage.evidence_ids)
                if (evidence_ids.find(evidence_id) == evidence_ids.end())
                    throw std::invalid_argument("stage references unknown evidence: " + evidence_id);
        return s;
    }

    std::string Phase4OperationsProjection::render_text(const Phase4OperationsSnapshot &s,
                                                        std::size_t width)
    {
        std::ostringstream out;
        out << "\n[PHASE 4 OPERATIONS] " << status_name(s.overall_status) << "  run=" << s.run_id
            << "  plan=r" << s.plan_revision << "  updated=" << s.updated_at << '\n';
        out << clipped(s.summary, width) << '\n';
        if (!s.blocker.empty())
            out << "BLOCKER: " << clipped(s.blocker, width - 9) << '\n';
        if (!s.residual_risk.empty())
            out << "RESIDUAL RISK: " << clipped(s.residual_risk, width - 15) << '\n';
        out << "Stages " << s.stages.size() << " | Evidence " << s.evidence.size() << " | Memory "
            << s.memory.size() << " | Invocations " << s.invocations.size() << " | HITL " << s.hitl.size() << '\n';
        for (const auto &stage : s.stages)
            out << "  [" << status_name(stage.status) << "] " << stage.label << " r" << stage.revision
                << " · " << clipped(stage.summary, width > 32 ? width - 32 : width) << '\n';
        for (const auto &request : s.hitl)
            out << "  [HITL " << status_name(request.status) << "] " << request.kind << " · "
                << clipped(request.summary, width > 28 ? width - 28 : width) << '\n';
        return out.str();
    }

    Phase4OperationsSnapshot Phase4OperationsProjection::demo_snapshot()
    {
        Phase4OperationsSnapshot s;
        s.snapshot_id = "ops-demo-001";
        s.tenant_id = "demo-tenant";
        s.run_id = "run-orbit-042";
        s.task_id = "task-gnss-ro-qa";
        s.updated_at = "2026-08-10T14:32:18+08:00";
        s.overall_status = OperationsStatus::Blocked;
        s.plan_revision = 3;
        s.summary = "Planning, memory and five-layer assurance completed; production release awaits one accountable approval.";
        s.blocker = "Architecture finding F-ARCH-07 requires manual review before release.";
        s.residual_risk = "Live provider certification is valid for staging only.";
        s.live_certification = "staging certified · production inconclusive";
        s.unknowns = {"Production KMS evidence not attached", "External domain benchmark pending"};
        s.stages = {{"intake", "Task cognition", OperationsStatus::Passed, 3, "analyst", "Requirements and boundaries frozen", {"EV-REQ-1"}},
                    {"plan", "Professional plan", OperationsStatus::Passed, 3, "planner", "12 executable tasks with acceptance criteria", {"EV-REQ-1"}},
                    {"execute", "Execution", OperationsStatus::Passed, 3, "executor", "Artifacts produced and digested", {"EV-BUILD-9"}},
                    {"assure", "Independent assurance", OperationsStatus::Warning, 2, "verifier", "One material architecture finding remains", {"EV-TEST-4", "EV-ARCH-7"}},
                    {"judge", "Judge & calibration", OperationsStatus::Passed, 1, "judge", "Agreement 0.91; no regression detected", {"EV-TEST-4"}},
                    {"release", "Release gate", OperationsStatus::Blocked, 1, "arbiter", "Waiting for accountable approval", {"EV-ARCH-7"}}};
        s.evidence = {{"EV-REQ-1", "Acceptance criteria are complete", "requirements", "plan store", "project", "2 min", OperationsStatus::Passed, "sha256:2a6f…19d1"},
                      {"EV-BUILD-9", "Artifacts reproduce in clean build", "build", "sandbox runner", "oracle", "1 min", OperationsStatus::Passed, "sha256:8c1b…993a"},
                      {"EV-TEST-4", "Functional and integration suites pass", "test", "ctest", "oracle", "1 min", OperationsStatus::Passed, "sha256:54e0…be81"},
                      {"EV-ARCH-7", "Recovery boundary needs review", "finding", "architecture verifier", "independent", "now", OperationsStatus::Warning, "sha256:7cd9…3aa4"}};
        s.memory = {{"MEM-SYS-2", "system", "AGENTS.md", "system", "current", "none", true, "Mandatory operating constraints"},
                    {"MEM-ORG-4", "organization", "engineering policy", "approved", "12 d", "none", true, "Release governance"},
                    {"MEM-PROJ-8", "project", "Phase 4 plan", "project", "1 h", "supersedes r2", true, "Current project baseline"},
                    {"MEM-TASK-11", "task", "assurance checkpoint", "runtime", "2 min", "F-ARCH-07 open", true, "Active blocker evidence"},
                    {"MEM-TURN-3", "turn", "retrieval candidate", "external", "3 y", "lower authority", false, "Excluded by freshness policy"}};
        s.invocations = {{"INV-AN-1", "analyst", "OpenAI", "analysis-model", "analyst@3", "planning-view@7", "none", 2180, 742, 0.0184, 1840, OperationsStatus::Passed},
                         {"INV-PL-2", "planner", "OpenAI", "planning-model", "planner@5", "planning-view@7", "none", 3910, 1288, 0.0412, 3260, OperationsStatus::Passed},
                         {"INV-VE-4", "architecture-verifier", "Anthropic", "verification-model", "arch@4", "assurance-view@2", "none", 2870, 904, 0.0331, 2940, OperationsStatus::Warning},
                         {"INV-JU-6", "judge", "OpenAI", "judge-model", "judge@2", "evaluation-view@3", "primary timeout → fallback", 1640, 512, 0.0197, 4120, OperationsStatus::Passed}};
        s.assurance = {{"functional", "Functional", OperationsStatus::Passed, "ctest", "functional-verifier", {}},
                       {"module", "Module", OperationsStatus::Passed, "contract suite", "code-verifier", {}},
                       {"integration", "Integration", OperationsStatus::Passed, "sandbox", "integration-verifier", {}},
                       {"comprehensive", "Comprehensive", OperationsStatus::Warning, "architecture rules", "architecture-verifier", {"F-ARCH-07"}},
                       {"metrics", "Metrics", OperationsStatus::Passed, "benchmark", "judge", {}}};
        s.hitl = {{"HITL-19", "manual_review", OperationsStatus::Pending, "Review recovery boundary and accept or request remediation", "release-arbiter", "2026-08-10T18:00+08:00", {"approve", "request_remediation", "reject"}}};
        return s;
    }

} // namespace agent_framework
