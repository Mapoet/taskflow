#include "agent/approval/executor.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::approval {
namespace {
namespace sqlite = internal::sqlite;

bool approver(const ReviewerIdentity& reviewer) {
    return reviewer.roles.count("approver") || reviewer.roles.count("admin");
}
bool high_risk(const ApprovalRequest& request) {
    return request.risk_level == "high" || request.risk_level == "critical";
}
ApprovalDecision final_decision(const ApprovalRequest& request, const ApprovalReview& review,
                                Decision decision) {
    ApprovalDecision out;
    out.metadata = request.metadata;
    out.approval_id = request.approval_id;
    out.request_digest = encode(request).at("canonical_digest").get<std::string>();
    out.reviewer_id = review.reviewer.principal_id;
    out.decision = decision;
    out.scope = request.scope;
    out.reason = review.reason;
    out.policy_revision = request.policy_revision;
    out.plan_digest = request.plan_digest;
    out.arguments_digest = request.arguments_digest;
    out.artifact_digest = request.artifact_digest;
    out.memory_view_digest = request.memory_view_digest;
    out.decided_at = review.decided_at;
    out.expires_at = request.expires_at;
    return out;
}
}

AccountableApprovalExecutor::AccountableApprovalExecutor(
    std::string path, ApprovalStore& store, PolicyDecisionPoint policy)
    : store_(store), policy_(std::move(policy)) {
    if(path.empty()) throw std::invalid_argument("approval executor journal path is required");
    const std::filesystem::path file(path); std::error_code ec;
    if(file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    sqlite3* opened = nullptr;
    if(ec || sqlite3_open_v2(path.c_str(), &opened, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                            SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const auto message = ec ? ec.message() : (opened ? sqlite3_errmsg(opened) : "sqlite open failed");
        if(opened) sqlite3_close(opened);
        throw std::runtime_error(message);
    }
    db_ = opened; sqlite3_busy_timeout(opened, 3000);
    sqlite::exec(opened, "PRAGMA journal_mode=WAL");
    sqlite::exec(opened, "PRAGMA synchronous=FULL");
    sqlite::exec(opened, "CREATE TABLE IF NOT EXISTS phase4_approval_votes("
        "approval_id TEXT NOT NULL,vote_revision INTEGER NOT NULL,request_digest TEXT NOT NULL,"
        "reviewer_id TEXT NOT NULL,delegated_by TEXT NOT NULL,decision INTEGER NOT NULL,"
        "reason TEXT NOT NULL,decided_at TEXT NOT NULL,PRIMARY KEY(approval_id,vote_revision),"
        "UNIQUE(approval_id,reviewer_id))");
#if !defined(_WIN32)
    std::filesystem::permissions(file, std::filesystem::perms::owner_read |
        std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, ec);
    if(ec) throw std::runtime_error(ec.message());
#endif
}

AccountableApprovalExecutor::~AccountableApprovalExecutor() {
    if(db_) sqlite3_close(sqlite::database(db_));
}

std::uint64_t AccountableApprovalExecutor::vote_revision(std::string_view approval_id) {
    std::lock_guard lock(mutex_); auto* db = sqlite::database(db_);
    sqlite::Statement query(db, "SELECT COALESCE(MAX(vote_revision),0) FROM phase4_approval_votes WHERE approval_id=?");
    sqlite::bind_text(query.get(), 1, approval_id);
    return sqlite::step(query.get()) == SQLITE_ROW
        ? static_cast<std::uint64_t>(sqlite::column_int64(query.get(), 0)) : 0;
}

ReviewResult AccountableApprovalExecutor::review(
    const ApprovalReview& review, std::uint64_t expected) {
    auto request = store_.request(review.approval_id);
    if(!request) return {ReviewOutcome::Denied, 0, 0, {}, "approval_not_found", {}};
    const auto expected_digest = encode(*request).at("canonical_digest").get<std::string>();
    if(review.request_digest != expected_digest)
        return {ReviewOutcome::Denied, 0, 0, {}, "stale_request_digest", {}};
    if(review.reviewer.principal_id.empty() || !approver(review.reviewer))
        return {ReviewOutcome::Denied, 0, 0, {}, "reviewer_not_authorized", {}};
    if(review.reviewer.principal_id == request->requester_id ||
       (!review.reviewer.delegated_by.empty() && review.reviewer.delegated_by == request->requester_id))
        return {ReviewOutcome::Denied, 0, 0, {}, "separation_of_duties_violation", {}};
    if(!review.reviewer.delegated_by.empty() &&
       !review.reviewer.delegated_scopes.count(request->scope))
        return {ReviewOutcome::Denied, 0, 0, {}, "delegation_scope_denied", {}};
    if(!request->expires_at.empty() && review.decided_at > request->expires_at)
        return {ReviewOutcome::Expired, expected, 0, {}, "approval_expired", {}};
    PolicyContext context;
    context.identity = request->metadata.identity;
    context.actor_id = review.reviewer.principal_id;
    context.actor_roles = review.reviewer.roles;
    context.action = "approval.review";
    context.resource = request->scope;
    context.effect_class = "governed_write";
    context.risk_level = request->risk_level;
    context.requester_id = request->requester_id;
    context.two_person_required = high_risk(*request);
    const auto evaluated = policy_.evaluate(context);
    if(evaluated.outcome == PolicyOutcome::Deny)
        return {ReviewOutcome::Denied, expected, 0, {}, "policy_denied", {}};

    std::lock_guard lock(mutex_); auto* db = sqlite::database(db_);
    try {
        sqlite::Transaction transaction(db);
        sqlite::Statement current(db, "SELECT COALESCE(MAX(vote_revision),0) FROM phase4_approval_votes WHERE approval_id=?");
        sqlite::bind_text(current.get(), 1, review.approval_id);
        sqlite::step(current.get());
        const auto revision = static_cast<std::uint64_t>(sqlite::column_int64(current.get(), 0));
        if(revision != expected)
            return {ReviewOutcome::RevisionConflict, revision, 0, {}, "vote_revision_conflict", {}};
        sqlite::Statement insert(db, "INSERT INTO phase4_approval_votes(approval_id,vote_revision,request_digest,"
            "reviewer_id,delegated_by,decision,reason,decided_at) VALUES(?,?,?,?,?,?,?,?)");
        sqlite::bind_text(insert.get(), 1, review.approval_id);
        sqlite::bind_int64(insert.get(), 2, static_cast<sqlite3_int64>(revision + 1));
        sqlite::bind_text(insert.get(), 3, review.request_digest);
        sqlite::bind_text(insert.get(), 4, review.reviewer.principal_id);
        sqlite::bind_text(insert.get(), 5, review.reviewer.delegated_by);
        sqlite::bind_int(insert.get(), 6, static_cast<int>(review.decision));
        sqlite::bind_text(insert.get(), 7, review.reason);
        sqlite::bind_text(insert.get(), 8, review.decided_at);
        if(sqlite::step(insert.get()) != SQLITE_DONE)
            return {ReviewOutcome::Denied, revision, 0, {}, "duplicate_or_invalid_reviewer", sqlite3_errmsg(db)};

        std::uint64_t approvals = 0;
        sqlite::Statement count(db, "SELECT COUNT(*) FROM phase4_approval_votes WHERE approval_id=? AND decision=?");
        sqlite::bind_text(count.get(), 1, review.approval_id);
        sqlite::bind_int(count.get(), 2, static_cast<int>(Decision::Approved));
        if(sqlite::step(count.get()) == SQLITE_ROW) approvals = sqlite::column_int64(count.get(), 0);
        transaction.commit();
        const auto vote_revision = revision + 1;
        const bool rejected = review.decision == Decision::Rejected;
        if(!rejected && approvals < (high_risk(*request) ? 2U : 1U))
            return {ReviewOutcome::AwaitingAdditionalReview, vote_revision, 0, {}, {}, {}};
        auto committed = store_.decide(final_decision(*request, review,
            rejected ? Decision::Rejected : Decision::Approved), 0);
        if(!committed)
            return {committed.status == ApprovalStoreStatus::RevisionConflict
                        ? ReviewOutcome::RevisionConflict : ReviewOutcome::Error,
                    vote_revision, committed.revision, {}, "decision_commit_failed", committed.error};
        return {rejected ? ReviewOutcome::Rejected : ReviewOutcome::Approved,
                vote_revision, committed.revision, committed.digest, {}, {}};
    } catch(const std::exception& e) {
        return {ReviewOutcome::Error, expected, 0, {}, "approval_executor_error", e.what()};
    }
}

ReviewResult AccountableApprovalExecutor::reconcile(
    std::string_view approval_id, std::string_view now) {
    auto request = store_.request(approval_id);
    if(!request) return {ReviewOutcome::Denied, 0, 0, {}, "approval_not_found", {}};
    if(auto existing = store_.latest_decision(approval_id)) {
        const auto digest = encode(*existing).at("canonical_digest").get<std::string>();
        const auto outcome = existing->decision == Decision::Approved ? ReviewOutcome::Approved
            : existing->decision == Decision::Rejected ? ReviewOutcome::Rejected
            : ReviewOutcome::Denied;
        return {outcome, vote_revision(approval_id), 1, digest, {}, {}};
    }
    if(!request->expires_at.empty() && now > request->expires_at)
        return {ReviewOutcome::Expired, vote_revision(approval_id), 0, {}, "approval_expired", {}};
    ApprovalReview last;
    std::uint64_t approvals = 0, revision = 0;
    {
        std::lock_guard lock(mutex_); auto* db = sqlite::database(db_);
        sqlite::Statement count(db, "SELECT COUNT(*),COALESCE(MAX(vote_revision),0) FROM phase4_approval_votes "
                                    "WHERE approval_id=? AND decision=?");
        sqlite::bind_text(count.get(), 1, approval_id);
        sqlite::bind_int(count.get(), 2, static_cast<int>(Decision::Approved));
        if(sqlite::step(count.get()) == SQLITE_ROW) {
            approvals = sqlite::column_int64(count.get(), 0);
            revision = sqlite::column_int64(count.get(), 1);
        }
        if(approvals < (high_risk(*request) ? 2U : 1U))
            return {ReviewOutcome::AwaitingAdditionalReview, revision, 0, {}, {}, {}};
        sqlite::Statement query(db, "SELECT reviewer_id,reason,decided_at FROM phase4_approval_votes "
                                    "WHERE approval_id=? AND decision=? ORDER BY vote_revision DESC LIMIT 1");
        sqlite::bind_text(query.get(), 1, approval_id);
        sqlite::bind_int(query.get(), 2, static_cast<int>(Decision::Approved));
        if(sqlite::step(query.get()) != SQLITE_ROW)
            return {ReviewOutcome::Error, revision, 0, {}, "approval_vote_corrupt", {}};
        last.approval_id = std::string(approval_id);
        last.request_digest = encode(*request).at("canonical_digest").get<std::string>();
        last.reviewer.principal_id = sqlite::column_text(query.get(), 0);
        last.reason = sqlite::column_text(query.get(), 1);
        last.decided_at = sqlite::column_text(query.get(), 2);
    }
    auto committed = store_.decide(final_decision(*request, last, Decision::Approved), 0);
    if(!committed) return {ReviewOutcome::Error, revision, committed.revision, {},
                           "decision_reconciliation_failed", committed.error};
    return {ReviewOutcome::Approved, revision, committed.revision, committed.digest, {}, {}};
}

RevisionResult AccountableApprovalExecutor::revise(
    std::string_view original_id, const ApprovalRequest& revised,
    const ReviewerIdentity& editor, std::string_view original_digest) {
    auto original = store_.request(original_id);
    if(!original) return {false, {}, "approval_not_found"};
    if(!approver(editor)) return {false, {}, "editor_not_authorized"};
    if(encode(*original).at("canonical_digest").get<std::string>() != original_digest)
        return {false, {}, "stale_request_digest"};
    if(revised.approval_id.empty() || revised.approval_id == original_id ||
       revised.metadata.identity.tenant_id != original->metadata.identity.tenant_id ||
       revised.metadata.identity.task_id != original->metadata.identity.task_id ||
       revised.proposed_change.empty())
        return {false, {}, "invalid_revised_request"};
    auto committed = store_.put_request(revised);
    return committed ? RevisionResult{true, committed.digest, {}}
                     : RevisionResult{false, {}, "revised_request_commit_failed"};
}

ApprovalHarnessPort::ApprovalHarnessPort(std::string id, ApprovalStore& store,
                                         std::string approval_id, std::string now)
    : id_(std::move(id)), store_(store), approval_id_(std::move(approval_id)), now_(std::move(now)) {
    if(id_.empty() || approval_id_.empty()) throw std::invalid_argument("approval harness port configuration is incomplete");
}

harness::HarnessStageResult ApprovalHarnessPort::execute(const harness::HarnessStageRequest& request) {
    harness::HarnessStageResult out;
    out.invocation_manifest_digest = request.request_digest;
    auto approval_request = store_.request(approval_id_);
    auto decision = store_.latest_decision(approval_id_);
    if(!approval_request) { out.outcome = harness::StageOutcome::ManualReview; out.error_code = "approval_request_missing"; return out; }
    const auto request_digest = encode(*approval_request).at("canonical_digest").get<std::string>();
    if(!decision) { out.outcome = harness::StageOutcome::AwaitingApproval; out.output_digest = request_digest; return out; }
    if(decision->request_digest != request_digest || decision->plan_digest != request.checkpoint.pins.plan_digest) {
        out.outcome = harness::StageOutcome::ManualReview; out.error_code = "approval_binding_mismatch"; return out;
    }
    if((!decision->expires_at.empty() && now_ > decision->expires_at) || decision->decision == Decision::Expired ||
       decision->decision == Decision::Revoked) {
        out.outcome = harness::StageOutcome::Rejected; out.error_code = "approval_inactive"; return out;
    }
    if(decision->decision != Decision::Approved) {
        out.outcome = harness::StageOutcome::Rejected; out.error_code = "approval_rejected"; return out;
    }
    out.outcome = harness::StageOutcome::Succeeded;
    out.output_digest = encode(*decision).at("canonical_digest").get<std::string>();
    out.pins.approval_decision_id = approval_id_ + ":" + out.output_digest;
    return out;
}
}  // namespace agent_framework::approval
