#include <agent/toolbus/tool_effect_journal.hpp>
#include <agent/internal/platform_io.hpp>
#include <agent/observability/audit.hpp>

#include <fstream>
#include <cerrno>
#include <sstream>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace agent_framework {
namespace {

std::string status_name(ToolEffectStatus status) {
    switch (status) {
        case ToolEffectStatus::Started: return "started";
        case ToolEffectStatus::Completed: return "completed";
        case ToolEffectStatus::Committed: return "committed";
        case ToolEffectStatus::ManualReview: return "manual_review";
        case ToolEffectStatus::Failed: return "failed";
        case ToolEffectStatus::Cancelled: return "cancelled";
    }
    return "failed";
}
ToolEffectStatus parse_status(const std::string& value) {
    if (value == "started") return ToolEffectStatus::Started;
    if (value == "completed") return ToolEffectStatus::Completed;
    if (value == "committed") return ToolEffectStatus::Committed;
    if (value == "manual_review") return ToolEffectStatus::ManualReview;
    if (value == "failed") return ToolEffectStatus::Failed;
    if (value == "cancelled") return ToolEffectStatus::Cancelled;
    throw std::runtime_error("tool effect WAL contains unknown status");
}
std::string policy_name(ToolReconciliationPolicy policy) {
    switch (policy) {
        case ToolReconciliationPolicy::ReplayIdempotent: return "replay_idempotent";
        case ToolReconciliationPolicy::LookupExternal: return "lookup_external";
        case ToolReconciliationPolicy::ManualReview: return "manual_review";
        case ToolReconciliationPolicy::FailClosed: return "fail_closed";
    }
    return "fail_closed";
}
ToolReconciliationPolicy parse_policy(const std::string& value) {
    if (value == "replay_idempotent") return ToolReconciliationPolicy::ReplayIdempotent;
    if (value == "lookup_external") return ToolReconciliationPolicy::LookupExternal;
    if (value == "manual_review") return ToolReconciliationPolicy::ManualReview;
    return ToolReconciliationPolicy::FailClosed;
}
json record_json(const ToolEffectRecord& record) {
    return {{"v", 1}, {"journal_sequence", record.journal_sequence},
            {"task_id", record.task_id}, {"session_id", record.session_id},
            {"tool_name", record.tool_name}, {"tool_call_id", record.tool_call_id},
            {"idempotency_key", record.idempotency_key}, {"request_digest", record.request_digest},
            {"result_digest", record.result_digest}, {"attempt", record.attempt},
            {"status", status_name(record.status)}, {"policy", policy_name(record.reconciliation_policy)},
            {"safe_to_replay", record.safe_to_replay}, {"started_at", record.started_at},
            {"completed_at", record.completed_at}, {"committed_at", record.committed_at},
            {"updated_at", record.updated_at}, {"error_code", record.error_code}};
}
ToolEffectRecord parse_record(const json& value) {
    if (!value.is_object() || value.value("v", 0) != 1)
        throw std::runtime_error("tool effect WAL has unsupported schema");
    ToolEffectRecord record;
    record.journal_sequence = value.at("journal_sequence").get<std::uint64_t>();
    record.task_id = value.value("task_id", "");
    record.session_id = value.value("session_id", "");
    record.tool_name = value.value("tool_name", "");
    record.tool_call_id = value.value("tool_call_id", "");
    record.idempotency_key = value.value("idempotency_key", "");
    record.request_digest = value.value("request_digest", "");
    record.result_digest = value.value("result_digest", "");
    record.attempt = value.value("attempt", std::size_t{0});
    record.status = parse_status(value.value("status", ""));
    record.reconciliation_policy = parse_policy(value.value("policy", "fail_closed"));
    record.safe_to_replay = value.value("safe_to_replay", false);
    record.started_at = value.value("started_at", "");
    record.completed_at = value.value("completed_at", "");
    record.committed_at = value.value("committed_at", "");
    record.updated_at = value.value("updated_at", "");
    record.error_code = value.value("error_code", "");
    if (record.idempotency_key.empty()) throw std::runtime_error("tool effect WAL contains empty key");
    return record;
}

} // namespace

ToolEffectJournal::ToolEffectJournal(std::filesystem::path wal_path)
    : wal_path_(std::move(wal_path)) {
    if (wal_path_->empty()) throw std::invalid_argument("tool effect WAL path cannot be empty");
    std::lock_guard<std::mutex> lock(mutex_);
    load_locked();
}

ToolEffectBeginResult ToolEffectJournal::begin(ToolEffectRecord record) {
    if (record.idempotency_key.empty() || record.request_digest.empty())
        return ToolEffectBeginResult::Rejected;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = by_key_.find(record.idempotency_key);
    if (existing != by_key_.end()) {
        if (existing->second.request_digest != record.request_digest)
            return ToolEffectBeginResult::Conflict;
        return existing->second.status == ToolEffectStatus::Committed
            ? ToolEffectBeginResult::ExistingCommitted : ToolEffectBeginResult::ExistingInFlight;
    }
    record.status = ToolEffectStatus::Started;
    record.started_at = record.updated_at = audit_timestamp_now();
    append_locked(record);
    by_key_.emplace(record.idempotency_key, std::move(record));
    return ToolEffectBeginResult::Started;
}

bool ToolEffectJournal::transition(const std::string& key, ToolEffectStatus expected,
                                   ToolEffectStatus next, const std::string& detail) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_key_.find(key);
    if (it == by_key_.end() || it->second.status != expected) return false;
    ToolEffectRecord updated = it->second;
    updated.status = next;
    updated.updated_at = audit_timestamp_now();
    if (next == ToolEffectStatus::Completed) {
        updated.result_digest = detail;
        updated.completed_at = updated.updated_at;
    } else if (next == ToolEffectStatus::Committed) {
        updated.committed_at = updated.updated_at;
    } else {
        updated.error_code = detail;
    }
    append_locked(updated);
    it->second = std::move(updated);
    return true;
}
bool ToolEffectJournal::complete(const std::string& key, const std::string& digest) {
    return !digest.empty() && transition(key, ToolEffectStatus::Started, ToolEffectStatus::Completed, digest);
}
bool ToolEffectJournal::commit(const std::string& key) {
    return transition(key, ToolEffectStatus::Completed, ToolEffectStatus::Committed, "");
}
bool ToolEffectJournal::mark_manual_review(const std::string& key, const std::string& error) {
    return transition(key, ToolEffectStatus::Started, ToolEffectStatus::ManualReview, error) ||
           transition(key, ToolEffectStatus::Completed, ToolEffectStatus::ManualReview, error);
}
bool ToolEffectJournal::fail(const std::string& key, const std::string& error) {
    return transition(key, ToolEffectStatus::Started, ToolEffectStatus::Failed, error) ||
           transition(key, ToolEffectStatus::Completed, ToolEffectStatus::Failed, error);
}
bool ToolEffectJournal::cancel(const std::string& key, const std::string& error) {
    return transition(key, ToolEffectStatus::Started, ToolEffectStatus::Cancelled, error);
}
bool ToolEffectJournal::prepare_replay(const std::string& key, std::size_t attempt) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_key_.find(key);
    if (it == by_key_.end() || it->second.status != ToolEffectStatus::Started ||
        it->second.reconciliation_policy != ToolReconciliationPolicy::ReplayIdempotent ||
        !it->second.safe_to_replay || attempt < it->second.attempt) return false;
    ToolEffectRecord updated = it->second;
    updated.attempt = attempt;
    updated.updated_at = audit_timestamp_now();
    updated.error_code = "replay_after_recovery";
    append_locked(updated);
    it->second = std::move(updated);
    return true;
}

std::optional<ToolEffectRecord> ToolEffectJournal::find_idempotency(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_key_.find(key);
    return it == by_key_.end() ? std::nullopt : std::optional<ToolEffectRecord>(it->second);
}
std::vector<ToolEffectRecord> ToolEffectJournal::recoverable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ToolEffectRecord> result;
    for (const auto& [_, record] : by_key_)
        if (record.status == ToolEffectStatus::Started || record.status == ToolEffectStatus::Completed ||
            record.status == ToolEffectStatus::ManualReview) result.push_back(record);
    return result;
}
ToolReconciliationAction ToolEffectJournal::reconciliation_action(const ToolEffectRecord& record) const noexcept {
    if (record.status == ToolEffectStatus::Committed || record.status == ToolEffectStatus::Failed ||
        record.status == ToolEffectStatus::Cancelled) return ToolReconciliationAction::None;
    if (record.status == ToolEffectStatus::ManualReview) return ToolReconciliationAction::ManualReview;
    switch (record.reconciliation_policy) {
        case ToolReconciliationPolicy::ReplayIdempotent:
            return record.status == ToolEffectStatus::Started && record.safe_to_replay
                ? ToolReconciliationAction::Replay : ToolReconciliationAction::Lookup;
        case ToolReconciliationPolicy::LookupExternal: return ToolReconciliationAction::Lookup;
        case ToolReconciliationPolicy::ManualReview: return ToolReconciliationAction::ManualReview;
        case ToolReconciliationPolicy::FailClosed: return ToolReconciliationAction::FailClosed;
    }
    return ToolReconciliationAction::FailClosed;
}
std::size_t ToolEffectJournal::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return by_key_.size();
}

void ToolEffectJournal::append_locked(ToolEffectRecord& record) {
    record.journal_sequence = next_sequence_++;
    if (!wal_path_) return;
    const auto parent = wal_path_->parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    const std::string line = record_json(record).dump() + '\n';
#ifndef _WIN32
    const int fd = ::open(wal_path_->c_str(), O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
    if (fd < 0) throw std::runtime_error("cannot open tool effect WAL");
    bool ok = ::flock(fd, LOCK_EX) == 0;
    std::size_t offset = 0;
    while (ok && offset < line.size()) {
        const auto count = ::write(fd, line.data() + offset, line.size() - offset);
        if (count < 0) { if (errno == EINTR) continue; ok = false; break; }
        offset += static_cast<std::size_t>(count);
    }
    if (ok) ok = internal::sync_file(fd) == 0;
    (void)::flock(fd, LOCK_UN);
    (void)::close(fd);
    if (!ok) throw std::runtime_error("cannot durably append tool effect WAL");
#else
    std::ofstream out(*wal_path_, std::ios::app | std::ios::binary);
    if (!out.write(line.data(), static_cast<std::streamsize>(line.size())))
        throw std::runtime_error("cannot append tool effect WAL");
    out.flush();
#endif
}

void ToolEffectJournal::load_locked() {
    if (!wal_path_ || !std::filesystem::exists(*wal_path_)) return;
    std::ifstream in(*wal_path_);
    std::string line;
    std::uint64_t last_sequence = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        ToolEffectRecord record = parse_record(json::parse(line));
        if (record.journal_sequence <= last_sequence)
            throw std::runtime_error("tool effect WAL sequence is not monotonic");
        last_sequence = record.journal_sequence;
        by_key_[record.idempotency_key] = std::move(record);
    }
    if (!in.eof()) throw std::runtime_error("cannot read tool effect WAL");
    next_sequence_ = last_sequence + 1;
}

} // namespace agent_framework
