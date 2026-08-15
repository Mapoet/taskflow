#ifndef AGENT_UI_PHASE4_OPERATIONS_HPP
#define AGENT_UI_PHASE4_OPERATIONS_HPP

#include <agent/core/types.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace agent_framework {

enum class OperationsStatus { Unknown, Pending, Running, Passed, Warning, Blocked, Failed };

struct OperationsStage {
    std::string id;
    std::string label;
    OperationsStatus status{OperationsStatus::Unknown};
    std::uint64_t revision{0};
    std::string role;
    std::string summary;
    std::vector<std::string> evidence_ids;
};

struct OperationsEvidence {
    std::string id;
    std::string claim;
    std::string kind;
    std::string source;
    std::string authority;
    std::string freshness;
    OperationsStatus status{OperationsStatus::Unknown};
    std::string digest;
};

struct OperationsMemoryItem {
    std::string id;
    std::string scope;
    std::string source;
    std::string authority;
    std::string freshness;
    std::string conflict;
    bool selected{false};
    std::string selection_reason;
};

struct OperationsInvocation {
    std::string id;
    std::string role;
    std::string provider;
    std::string model;
    std::string prompt_version;
    std::string view_id;
    std::string fallback;
    std::uint64_t input_tokens{0};
    std::uint64_t output_tokens{0};
    double cost_usd{0.0};
    std::int64_t latency_ms{0};
    OperationsStatus status{OperationsStatus::Unknown};
};

struct OperationsAssuranceLayer {
    std::string id;
    std::string label;
    OperationsStatus status{OperationsStatus::Unknown};
    std::string oracle;
    std::string verifier;
    std::vector<std::string> finding_ids;
};

struct OperationsHitlRequest {
    std::string id;
    std::string kind;
    OperationsStatus status{OperationsStatus::Pending};
    std::string summary;
    std::string requested_by;
    std::string deadline;
    std::vector<std::string> allowed_actions;
};

struct OperationsSourceRevision {
    std::string store;
    std::string object_id;
    std::uint64_t revision{0};
    std::string digest;
};

struct OperationsAgentTemplate {
    std::string template_id;
    std::uint64_t template_revision{0};
    std::string template_digest;
    std::string invocation_id;
    std::string business_mode;
    std::string hosting_mode;
    std::string plan_id;
    std::uint64_t plan_revision{0};
    std::string plan_digest;
    std::string session_id;
    std::string session_digest;
    std::string registry_generation;
    std::string deployment_generation;
    std::string completion_authority;
    std::string completion_reason;
};

struct OperationsSkillNode {
    std::string node_id;
    std::string skill_id;
    std::string skill_version;
    std::string runner;
    std::string role;
    std::string state;
    std::string output_digest;
    std::vector<std::string> evidence_refs;
    std::vector<std::string> artifact_refs;
};

/**
 * Canonical, display-safe Phase 4 control-plane projection.
 *
 * It intentionally contains prompt/view identifiers and displayable summaries only. Raw prompts,
 * credentials, hidden reasoning, tool payloads, and memory contents are outside this contract.
 */
struct Phase4OperationsSnapshot {
    std::string schema_version{"phase4.operations.v1"};
    std::string snapshot_id;
    std::string tenant_id;
    std::string run_id;
    std::string task_id;
    std::string conversation_id;
    std::string turn_id;
    std::string updated_at;
    OperationsStatus overall_status{OperationsStatus::Unknown};
    std::uint64_t plan_revision{0};
    std::string summary;
    std::string blocker;
    std::string residual_risk;
    std::string live_certification;
    std::string task_closure_state{"running"};
    std::string task_closure_reason;
    std::string completion_authority{"none"};
    bool task_completion_verified{false};
    std::int64_t progress_delta{0};
    std::uint64_t stagnation_count{0};
    std::uint64_t criteria_closed{0};
    std::uint64_t criteria_total{0};
    std::uint64_t invocations_compacted{0};
    double cost_per_closed_criterion{0.0};
    std::vector<std::string> unknowns;
    std::vector<OperationsStage> stages;
    std::vector<OperationsEvidence> evidence;
    std::vector<OperationsMemoryItem> memory;
    std::vector<OperationsInvocation> invocations;
    std::vector<OperationsAssuranceLayer> assurance;
    std::vector<OperationsHitlRequest> hitl;
    std::vector<OperationsSourceRevision> source_revisions;
    std::vector<OperationsAgentTemplate> agent_templates;
    std::vector<OperationsSkillNode> skill_nodes;
};

class Phase4OperationsProjection {
public:
    static constexpr std::string_view event_type{"phase4_operations"};
    static constexpr std::size_t max_items{256};

    static const char* status_name(OperationsStatus status) noexcept;
    static OperationsStatus parse_status(std::string_view value);
    static json to_json(const Phase4OperationsSnapshot& snapshot);
    static Phase4OperationsSnapshot from_json(const json& value);
    // Keeps active invocations and the newest terminal history while bounding
    // both invocation rows and their per-invocation replay cursors.
    static std::size_t compact_invocations(Phase4OperationsSnapshot& snapshot);
    static std::string render_text(const Phase4OperationsSnapshot& snapshot,
                                   std::size_t width = 100);
    static Phase4OperationsSnapshot demo_snapshot();
};

} // namespace agent_framework

#endif
