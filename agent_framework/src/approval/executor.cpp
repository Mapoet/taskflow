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
int risk_rank(std::string_view risk) {
    if(risk == "critical") return 4;
    if(risk == "high") return 3;
    if(risk == "medium") return 2;
    return 1;
}
bool has_column(sqlite3* db, const char* table, std::string_view column) {
    sqlite::Statement query(db, (std::string("PRAGMA table_info(") + table + ")").c_str());
    while(sqlite::step(query.get()) == SQLITE_ROW)
        if(sqlite::column_text(query.get(), 1) == column) return true;
    return false;
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
        "reason TEXT NOT NULL,decided_at TEXT NOT NULL,reviewer_roles_json TEXT NOT NULL DEFAULT '[]',"
        "identity_attestation TEXT NOT NULL DEFAULT '',PRIMARY KEY(approval_id,vote_revision),"
        "UNIQUE(approval_id,reviewer_id))");
    if(!has_column(opened, "phase4_approval_votes", "reviewer_roles_json"))
        sqlite::exec(opened, "ALTER TABLE phase4_approval_votes ADD COLUMN reviewer_roles_json TEXT NOT NULL DEFAULT '[]'");
    if(!has_column(opened, "phase4_approval_votes", "identity_attestation"))
        sqlite::exec(opened, "ALTER TABLE phase4_approval_votes ADD COLUMN identity_attestation TEXT NOT NULL DEFAULT ''");
    sqlite::exec(opened, "CREATE TABLE IF NOT EXISTS phase4_delegation_grants("
        "grant_id TEXT PRIMARY KEY,grantor_id TEXT NOT NULL,delegate_id TEXT NOT NULL,"
        "scopes_json TEXT NOT NULL,roles_json TEXT NOT NULL,maximum_risk TEXT NOT NULL,"
        "valid_from TEXT NOT NULL,expires_at TEXT NOT NULL,maximum_depth INTEGER NOT NULL,"
        "authority_attestation TEXT NOT NULL,revoked_at TEXT NOT NULL DEFAULT '')");
    sqlite::exec(opened, "CREATE TABLE IF NOT EXISTS phase4_approval_supersessions("
        "original_id TEXT PRIMARY KEY,revised_id TEXT NOT NULL,parent_digest TEXT NOT NULL,"
        "created_at TEXT NOT NULL)");
    sqlite::exec(opened, "CREATE TABLE IF NOT EXISTS phase4_approval_escalations("
        "escalation_id TEXT PRIMARY KEY,approval_id TEXT NOT NULL UNIQUE,target_group TEXT NOT NULL,"
        "reason TEXT NOT NULL,created_at TEXT NOT NULL)");
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
    if(superseded(review.approval_id))
        return {ReviewOutcome::Denied, 0, 0, {}, "approval_superseded", {}};
    if(review.reviewer.principal_id.empty() || !approver(review.reviewer))
        return {ReviewOutcome::Denied, 0, 0, {}, "reviewer_not_authorized", {}};
    if(review.reviewer.principal_id == request->requester_id ||
       (!review.reviewer.delegated_by.empty() && review.reviewer.delegated_by == request->requester_id))
        return {ReviewOutcome::Denied, 0, 0, {}, "separation_of_duties_violation", {}};
    if(!review.reviewer.delegated_by.empty()) {
        if(review.reviewer.delegation_grant_id.empty())
            return {ReviewOutcome::Denied, 0, 0, {}, "delegation_grant_required", {}};
        std::lock_guard grant_lock(mutex_); auto* grant_db = sqlite::database(db_);
        sqlite::Statement grant(grant_db, "SELECT grantor_id,delegate_id,scopes_json,roles_json,valid_from,expires_at,revoked_at,maximum_risk FROM phase4_delegation_grants WHERE grant_id=?");
        sqlite::bind_text(grant.get(), 1, review.reviewer.delegation_grant_id);
        if(sqlite::step(grant.get()) != SQLITE_ROW)
            return {ReviewOutcome::Denied, 0, 0, {}, "delegation_grant_not_found", {}};
        const auto scopes = nlohmann::json::parse(sqlite::column_text(grant.get(), 2)).get<std::set<std::string>>();
        const auto roles = nlohmann::json::parse(sqlite::column_text(grant.get(), 3)).get<std::set<std::string>>();
        if(sqlite::column_text(grant.get(), 0) != review.reviewer.delegated_by ||
           sqlite::column_text(grant.get(), 1) != review.reviewer.principal_id || !scopes.count(request->scope) ||
           !roles.count("approver") || (!sqlite::column_text(grant.get(), 4).empty() && review.decided_at < sqlite::column_text(grant.get(), 4)) ||
           (!sqlite::column_text(grant.get(), 5).empty() && review.decided_at > sqlite::column_text(grant.get(), 5)) ||
           !sqlite::column_text(grant.get(), 6).empty() ||
           risk_rank(request->risk_level) > risk_rank(sqlite::column_text(grant.get(), 7)))
            return {ReviewOutcome::Denied, 0, 0, {}, "delegation_scope_denied", {}};
    }
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
            "reviewer_id,delegated_by,decision,reason,decided_at,reviewer_roles_json,identity_attestation) VALUES(?,?,?,?,?,?,?,?,?,?)");
        sqlite::bind_text(insert.get(), 1, review.approval_id);
        sqlite::bind_int64(insert.get(), 2, static_cast<sqlite3_int64>(revision + 1));
        sqlite::bind_text(insert.get(), 3, review.request_digest);
        sqlite::bind_text(insert.get(), 4, review.reviewer.principal_id);
        sqlite::bind_text(insert.get(), 5, review.reviewer.delegated_by);
        sqlite::bind_int(insert.get(), 6, static_cast<int>(review.decision));
        sqlite::bind_text(insert.get(), 7, review.reason);
        sqlite::bind_text(insert.get(), 8, review.decided_at);
        sqlite::bind_text(insert.get(), 9, nlohmann::json(review.reviewer.roles).dump());
        sqlite::bind_text(insert.get(), 10, review.reviewer.identity_attestation);
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
        const auto requested_quorum = request->proposed_change.value("required_quorum", high_risk(*request) ? 2U : 1U);
        std::set<std::string> represented_roles;
        sqlite::Statement role_query(db, "SELECT reviewer_roles_json FROM phase4_approval_votes WHERE approval_id=? AND decision=?");
        sqlite::bind_text(role_query.get(), 1, review.approval_id); sqlite::bind_int(role_query.get(), 2, static_cast<int>(Decision::Approved));
        while(sqlite::step(role_query.get()) == SQLITE_ROW) {
            const auto roles = nlohmann::json::parse(sqlite::column_text(role_query.get(), 0)).get<std::set<std::string>>();
            represented_roles.insert(roles.begin(), roles.end());
        }
        bool required_roles_met = true;
        for(const auto& role : request->proposed_change.value("required_roles", std::vector<std::string>{}))
            required_roles_met = required_roles_met && represented_roles.count(role);
        if(!rejected && (approvals < requested_quorum || !required_roles_met))
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
        const auto requested_quorum = request->proposed_change.value("required_quorum", high_risk(*request) ? 2U : 1U);
        if(approvals < requested_quorum)
            return {ReviewOutcome::AwaitingAdditionalReview, revision, 0, {}, {}, {}};
        std::set<std::string> represented_roles;
        sqlite::Statement roles(db, "SELECT reviewer_roles_json FROM phase4_approval_votes WHERE approval_id=? AND decision=?");
        sqlite::bind_text(roles.get(), 1, approval_id); sqlite::bind_int(roles.get(), 2, static_cast<int>(Decision::Approved));
        while(sqlite::step(roles.get()) == SQLITE_ROW) {
            const auto values = nlohmann::json::parse(sqlite::column_text(roles.get(), 0)).get<std::set<std::string>>();
            represented_roles.insert(values.begin(), values.end());
        }
        for(const auto& role : request->proposed_change.value("required_roles", std::vector<std::string>{}))
            if(!represented_roles.count(role))
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
    if(!committed) return {false, {}, "revised_request_commit_failed"};
    {
        std::lock_guard lock(mutex_); auto* db = sqlite::database(db_);
        sqlite::Statement insert(db, "INSERT INTO phase4_approval_supersessions(original_id,revised_id,parent_digest,created_at) VALUES(?,?,?,strftime('%Y-%m-%dT%H:%M:%fZ','now'))");
        sqlite::bind_text(insert.get(), 1, original_id); sqlite::bind_text(insert.get(), 2, revised.approval_id);
        sqlite::bind_text(insert.get(), 3, original_digest);
        if(sqlite::step(insert.get()) != SQLITE_DONE) return {false, {}, "supersession_commit_failed"};
    }
    return {true, committed.digest, {}};
}

bool AccountableApprovalExecutor::grant_delegation(const DelegationGrant& grant, std::string* error) {
    if(grant.grant_id.empty() || grant.grantor_id.empty() || grant.delegate_id.empty() ||
       grant.grantor_id == grant.delegate_id || grant.scopes.empty() || !grant.roles.count("approver") ||
       grant.maximum_depth == 0) { if(error) *error = "invalid delegation grant"; return false; }
    std::lock_guard lock(mutex_); auto* db = sqlite::database(db_);
    sqlite::Statement insert(db, "INSERT INTO phase4_delegation_grants(grant_id,grantor_id,delegate_id,scopes_json,roles_json,maximum_risk,valid_from,expires_at,maximum_depth,authority_attestation) VALUES(?,?,?,?,?,?,?,?,?,?)");
    sqlite::bind_text(insert.get(), 1, grant.grant_id); sqlite::bind_text(insert.get(), 2, grant.grantor_id);
    sqlite::bind_text(insert.get(), 3, grant.delegate_id); sqlite::bind_text(insert.get(), 4, nlohmann::json(grant.scopes).dump());
    sqlite::bind_text(insert.get(), 5, nlohmann::json(grant.roles).dump()); sqlite::bind_text(insert.get(), 6, grant.maximum_risk);
    sqlite::bind_text(insert.get(), 7, grant.valid_from); sqlite::bind_text(insert.get(), 8, grant.expires_at);
    sqlite::bind_uint64(insert.get(), 9, grant.maximum_depth); sqlite::bind_text(insert.get(), 10, grant.authority_attestation);
    if(sqlite::step(insert.get()) != SQLITE_DONE) { if(error) *error = sqlite3_errmsg(db); return false; }
    return true;
}

bool AccountableApprovalExecutor::revoke_delegation(std::string_view id, std::string_view at, std::string* error) {
    std::lock_guard lock(mutex_); auto* db = sqlite::database(db_);
    sqlite::Statement update(db, "UPDATE phase4_delegation_grants SET revoked_at=? WHERE grant_id=? AND revoked_at=''");
    sqlite::bind_text(update.get(), 1, at); sqlite::bind_text(update.get(), 2, id);
    if(sqlite::step(update.get()) != SQLITE_DONE || sqlite::changes(db) != 1) { if(error) *error = "grant not found or already revoked"; return false; }
    return true;
}

bool AccountableApprovalExecutor::escalate(const EscalationRecord& value, std::string* error) {
    if(value.escalation_id.empty() || value.approval_id.empty() || value.target_group.empty() || value.reason.empty()) {
        if(error) *error = "invalid escalation";
        return false;
    }
    if(!store_.request(value.approval_id) || store_.latest_decision(value.approval_id) || superseded(value.approval_id)) {
        if(error) *error = "approval is not pending";
        return false;
    }
    std::lock_guard lock(mutex_); auto* db = sqlite::database(db_);
    sqlite::Statement insert(db, "INSERT INTO phase4_approval_escalations(escalation_id,approval_id,target_group,reason,created_at) VALUES(?,?,?,?,?)");
    sqlite::bind_text(insert.get(), 1, value.escalation_id); sqlite::bind_text(insert.get(), 2, value.approval_id);
    sqlite::bind_text(insert.get(), 3, value.target_group); sqlite::bind_text(insert.get(), 4, value.reason);
    sqlite::bind_text(insert.get(), 5, value.created_at);
    if(sqlite::step(insert.get()) != SQLITE_DONE) { if(error) *error = sqlite3_errmsg(db); return false; }
    return true;
}

std::optional<EscalationRecord> AccountableApprovalExecutor::escalation(std::string_view approval_id) {
    std::lock_guard lock(mutex_); auto* db = sqlite::database(db_);
    sqlite::Statement query(db, "SELECT escalation_id,target_group,reason,created_at FROM phase4_approval_escalations WHERE approval_id=?");
    sqlite::bind_text(query.get(), 1, approval_id); if(sqlite::step(query.get()) != SQLITE_ROW) return std::nullopt;
    return EscalationRecord{sqlite::column_text(query.get(), 0), std::string(approval_id), sqlite::column_text(query.get(), 1), sqlite::column_text(query.get(), 2), sqlite::column_text(query.get(), 3)};
}

bool AccountableApprovalExecutor::superseded(std::string_view approval_id) {
    std::lock_guard lock(mutex_); auto* db = sqlite::database(db_);
    sqlite::Statement query(db, "SELECT 1 FROM phase4_approval_supersessions WHERE original_id=?");
    sqlite::bind_text(query.get(), 1, approval_id); return sqlite::step(query.get()) == SQLITE_ROW;
}

ReviewResult AuthenticatedApprovalActionService::submit(
    const AuthenticatedPrincipal& principal, const ApprovalActionIntent& intent) {
    if(principal.principal_id.empty() || principal.identity_attestation.empty())
        return {ReviewOutcome::Denied, 0, 0, {}, "authenticated_identity_required", {}};
    if(intent.action != "approve" && intent.action != "reject" &&
       intent.action != "request_remediation")
        return {ReviewOutcome::Denied, 0, 0, {}, "unsupported_approval_action", {}};
    ApprovalReview review;
    review.approval_id = intent.approval_id;
    review.request_digest = intent.request_digest;
    review.reviewer.principal_id = principal.principal_id;
    review.reviewer.roles = principal.roles;
    review.reviewer.identity_attestation = principal.identity_attestation;
    review.decision = intent.action == "approve" ? Decision::Approved : Decision::Rejected;
    review.reason = intent.reason;
    review.decided_at = intent.decided_at;
    return executor_.review(review, intent.expected_vote_revision);
}

ApprovalAdministrativeResult AuthenticatedApprovalActionService::revise(
    const AuthenticatedPrincipal& principal,const ApprovalRevisionIntent& intent){
    if(principal.principal_id.empty()||principal.identity_attestation.empty())return {false,{},"authenticated_identity_required",{}};
    ReviewerIdentity editor{principal.principal_id,principal.roles,{},{},{},principal.identity_attestation};
    auto result=executor_.revise(intent.original_approval_id,intent.revised_request,editor,intent.original_request_digest);
    return {result.committed,result.request_digest,result.error_code,{}};
}
ApprovalAdministrativeResult AuthenticatedApprovalActionService::delegate(
    const AuthenticatedPrincipal& principal,const ApprovalDelegationIntent& intent){
    if(principal.principal_id.empty()||principal.identity_attestation.empty())return {false,{},"authenticated_identity_required",{}};
    if(intent.grant.grantor_id!=principal.principal_id)return {false,{},"delegation_grantor_mismatch",{}};
    std::string error;const bool ok=executor_.grant_delegation(intent.grant,&error);
    return {ok,contracts::canonical_digest(nlohmann::json{{"grant_id",intent.grant.grant_id},{"delegate_id",intent.grant.delegate_id},{"scopes",intent.grant.scopes}}).value_or(""),ok?std::string{}:"delegation_rejected",error};
}
ApprovalAdministrativeResult AuthenticatedApprovalActionService::escalate(
    const AuthenticatedPrincipal& principal,const ApprovalEscalationIntent& intent){
    if(principal.principal_id.empty()||principal.identity_attestation.empty())return {false,{},"authenticated_identity_required",{}};
    if(!principal.roles.count("approver")&&!principal.roles.count("approval_admin"))return {false,{},"escalation_role_required",{}};
    std::string error;const bool ok=executor_.escalate(intent.escalation,&error);
    return {ok,contracts::canonical_digest(nlohmann::json{{"escalation_id",intent.escalation.escalation_id},{"approval_id",intent.escalation.approval_id},{"target_group",intent.escalation.target_group}}).value_or(""),ok?std::string{}:"escalation_rejected",error};
}

ApprovalHarnessPort::ApprovalHarnessPort(std::string id, ApprovalStore& store,
                                         std::string approval_id, std::string now)
    : id_(std::move(id)), store_(store), approval_id_(std::move(approval_id)), now_(std::move(now)) {
    if(id_.empty() || approval_id_.empty()) throw std::invalid_argument("approval harness port configuration is incomplete");
}

std::string ApprovalHarnessPort::capability_manifest_digest() const {
    return contracts::canonical_digest(nlohmann::json{{"port", id_}, {"kind", "approval_store_pdp"},
        {"approval_id", approval_id_}}).value_or("");
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
