#include "agent/recovery/system_state_reconciler.hpp"

#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

#include "agent/contracts/contract.hpp"

namespace agent_framework::recovery {
namespace {

bool terminal(conversation::TurnPhase phase) {
    return phase == conversation::TurnPhase::Completed ||
           phase == conversation::TurnPhase::Failed;
}

std::string turn_from_harness(std::string_view harness_id) {
    constexpr std::string_view prefix = "turn:";
    return harness_id.starts_with(prefix)
        ? std::string(harness_id.substr(prefix.size())) : std::string{};
}

bool pending_effect(const harness::HarnessCheckpoint& checkpoint) {
    return std::any_of(checkpoint.outbox.begin(), checkpoint.outbox.end(),
                       [](const auto& item) {
                           return item.state == harness::OutboxState::Pending ||
                                  item.state == harness::OutboxState::Unknown;
                       });
}

std::string plan_digest(const ReconciliationPlan& plan) {
    nlohmann::json findings = nlohmann::json::array();
    for (const auto& finding : plan.findings) {
        findings.push_back({{"code", finding.code},
                            {"disposition", name(finding.disposition)},
                            {"turn_id", finding.turn_id},
                            {"harness_id", finding.harness_id},
                            {"turn_revision", finding.turn_revision},
                            {"harness_revision", finding.harness_revision},
                            {"reason", finding.reason}});
    }
    return contracts::canonical_digest({
        {"tenant_id", plan.scope.conversation.tenant_id},
        {"conversation_id", plan.scope.conversation.conversation_id},
        {"findings", std::move(findings)}}).value_or("");
}

} // namespace

std::string_view name(ReconciliationDisposition disposition) noexcept {
    switch (disposition) {
        case ReconciliationDisposition::Recoverable: return "recoverable";
        case ReconciliationDisposition::FailTerminal: return "fail_terminal";
        case ReconciliationDisposition::AwaitingExternal: return "awaiting_external";
        case ReconciliationDisposition::ManualReview: return "manual_review";
    }
    return "manual_review";
}

ReconciliationPlan SystemStateReconciler::scan(const ReconciliationScope& scope) const {
    if (scope.conversation.tenant_id.empty() ||
        scope.conversation.conversation_id.empty() || scope.limit == 0)
        throw std::invalid_argument("reconciliation scope identity and limit are required");

    ReconciliationPlan plan;
    plan.scope = scope;
    const auto turns = conversations_.list_turns(scope.conversation, false, scope.limit);
    std::unordered_map<std::string, conversation::TurnCheckpoint> by_id;
    for (const auto& turn : turns) {
        by_id.emplace(turn.turn_id, turn);
        if (!terminal(turn.phase) && turn.iteration == 0) {
            plan.findings.push_back({
                "conversation_zero_iteration_nonterminal",
                ReconciliationDisposition::ManualReview,
                turn.turn_id, {}, turn.revision, 0,
                "nonterminal turn has never crossed a model execution boundary"});
        }
    }

    const auto checkpoints = harnesses_.list_recoverable(
        scope.conversation.tenant_id, scope.limit);
    for (const auto& stored : checkpoints) {
        const auto& checkpoint = stored.checkpoint;
        const auto extension = checkpoint.metadata.extensions.find("conversation_id");
        if (extension == checkpoint.metadata.extensions.end() || !extension->is_string() ||
            extension->get<std::string>() != scope.conversation.conversation_id)
            continue;
        const std::string turn_id = turn_from_harness(checkpoint.harness_id);
        const auto found = by_id.find(turn_id);
        if (turn_id.empty() || found == by_id.end()) {
            plan.findings.push_back({
                "harness_without_conversation_turn",
                ReconciliationDisposition::ManualReview,
                turn_id, checkpoint.harness_id, 0, stored.revision,
                "recoverable harness cannot be correlated to a durable turn"});
            continue;
        }
        const auto& turn = found->second;
        if (terminal(turn.phase)) {
            plan.findings.push_back({
                "terminal_conversation_with_recoverable_harness",
                pending_effect(checkpoint) ? ReconciliationDisposition::ManualReview
                                           : ReconciliationDisposition::FailTerminal,
                turn_id, checkpoint.harness_id, turn.revision, stored.revision,
                pending_effect(checkpoint)
                    ? "pending or unknown execution effect requires reconciliation"
                    : "conversation is terminal while harness is recoverable"});
        } else if (pending_effect(checkpoint)) {
            plan.findings.push_back({
                "unbound_pending_harness_effect",
                ReconciliationDisposition::ManualReview,
                turn_id, checkpoint.harness_id, turn.revision, stored.revision,
                "pending harness effect has no durable invocation binding"});
        }
    }
    std::sort(plan.findings.begin(), plan.findings.end(), [](const auto& left,
                                                             const auto& right) {
        return std::tie(left.turn_id, left.harness_id, left.code) <
               std::tie(right.turn_id, right.harness_id, right.code);
    });
    plan.digest = plan_digest(plan);
    return plan;
}

ReconciliationApplyResult SystemStateReconciler::apply(
    const ReconciliationPlan& plan, bool dry_run) const {
    ReconciliationApplyResult result;
    result.inspected = plan.findings.size();
    if (plan.digest.empty() || plan.digest != plan_digest(plan)) {
        result.errors.push_back("reconciliation_plan_digest_mismatch");
        return result;
    }
    for (const auto& finding : plan.findings) {
        if (finding.harness_id.empty()) continue;
        if (finding.disposition != ReconciliationDisposition::ManualReview &&
            finding.disposition != ReconciliationDisposition::FailTerminal)
            continue;
        if (finding.disposition == ReconciliationDisposition::ManualReview)
            ++result.manual_review;
        if (dry_run) continue;
        auto stored = harnesses_.load(plan.scope.conversation.tenant_id,
                                      finding.harness_id);
        if (!stored) {
            result.errors.push_back(finding.harness_id + ":not_found");
            continue;
        }
        if (stored->revision != finding.harness_revision) {
            ++result.conflicts;
            continue;
        }
        auto checkpoint = stored->checkpoint;
        checkpoint.revision = stored->revision + 1;
        checkpoint.state = finding.disposition == ReconciliationDisposition::ManualReview
            ? harness::HarnessState::ManualReview : harness::HarnessState::Failed;
        checkpoint.terminal_reason = finding.code;
        for (auto& outbox : checkpoint.outbox) {
            if (outbox.state == harness::OutboxState::Pending) {
                outbox.state = harness::OutboxState::Unknown;
                outbox.error = "reconciliation_required";
            }
        }
        harness::HarnessEvent event;
        event.harness_id = checkpoint.harness_id;
        event.sequence = checkpoint.revision;
        event.checkpoint_revision = checkpoint.revision;
        event.event_type = "system_reconciled";
        event.created_at = checkpoint.updated_at;
        event.payload = {{"finding_code", finding.code},
                         {"disposition", name(finding.disposition)},
                         {"plan_digest", plan.digest},
                         {"turn_id", finding.turn_id}};
        const auto commit = harnesses_.compare_exchange(
            checkpoint, stored->revision, event);
        if (commit.status == harness::HarnessStoreStatus::RevisionConflict) {
            ++result.conflicts;
        } else if (!commit) {
            result.errors.push_back(finding.harness_id + ":" + commit.error);
        } else {
            ++result.changed;
        }
    }
    return result;
}

} // namespace agent_framework::recovery
