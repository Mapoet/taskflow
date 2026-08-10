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

/**
 * Canonical, display-safe Phase 4 control-plane projection.
 *
 * It intentionally contains prompt/view identifiers and displayable summaries only. Raw prompts,
 * credentials, hidden reasoning, tool payloads, and memory contents are outside this contract.
 */
struct Phase4OperationsSnapshot {
    std::string schema_version{"phase4.operations.v1"};
    std::string snapshot_id;
    std::string run_id;
    std::string task_id;
    std::string updated_at;
    OperationsStatus overall_status{OperationsStatus::Unknown};
    std::uint64_t plan_revision{0};
    std::string summary;
    std::string blocker;
    std::string residual_risk;
    std::string live_certification;
    std::vector<std::string> unknowns;
    std::vector<OperationsStage> stages;
    std::vector<OperationsEvidence> evidence;
    std::vector<OperationsMemoryItem> memory;
    std::vector<OperationsInvocation> invocations;
    std::vector<OperationsAssuranceLayer> assurance;
    std::vector<OperationsHitlRequest> hitl;
};

class Phase4OperationsProjection {
public:
    static constexpr std::string_view event_type{"phase4_operations"};

    static const char* status_name(OperationsStatus status) noexcept;
    static OperationsStatus parse_status(std::string_view value);
    static json to_json(const Phase4OperationsSnapshot& snapshot);
    static Phase4OperationsSnapshot from_json(const json& value);
    static std::string render_text(const Phase4OperationsSnapshot& snapshot,
                                   std::size_t width = 100);
    static Phase4OperationsSnapshot demo_snapshot();
};

} // namespace agent_framework

#endif
