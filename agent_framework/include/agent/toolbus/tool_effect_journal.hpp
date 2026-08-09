#ifndef AGENT_TOOL_EFFECT_JOURNAL_HPP
#define AGENT_TOOL_EFFECT_JOURNAL_HPP

#include <agent/core/types.hpp>

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace agent_framework {

enum class ToolEffectStatus { Started, Completed, Committed, ManualReview, Failed, Cancelled };
enum class ToolReconciliationPolicy { ReplayIdempotent, LookupExternal, ManualReview, FailClosed };
enum class ToolReconciliationAction { None, Replay, Lookup, ManualReview, FailClosed };
enum class ToolEffectBeginResult { Started, ExistingCommitted, ExistingInFlight, Conflict, Rejected };

struct ToolEffectRecord {
    std::string task_id;
    std::string session_id;
    std::string tool_name;
    std::string tool_call_id;
    std::string idempotency_key;
    std::string request_digest;
    std::string result_digest;
    std::size_t attempt{0};
    ToolEffectStatus status{ToolEffectStatus::Started};
    ToolReconciliationPolicy reconciliation_policy{ToolReconciliationPolicy::FailClosed};
    bool safe_to_replay{false};
    std::string started_at;
    std::string completed_at;
    std::string committed_at;
    std::string updated_at;
    std::string error_code;
    std::uint64_t journal_sequence{0};
};

class ToolEffectBlocked final : public std::runtime_error {
public:
    explicit ToolEffectBlocked(std::string reason) : std::runtime_error(std::move(reason)) {}
};

/**
 * Durable tool-effect state machine. With no path it is an in-memory test/store implementation;
 * with a path every accepted transition is appended and fsynced before becoming visible.
 */
class ToolEffectJournal {
public:
    ToolEffectJournal() = default;
    explicit ToolEffectJournal(std::filesystem::path wal_path);

    ToolEffectBeginResult begin(ToolEffectRecord record);
    bool start(ToolEffectRecord record) { return begin(std::move(record)) == ToolEffectBeginResult::Started; }
    bool complete(const std::string& idempotency_key, const std::string& result_digest);
    bool commit(const std::string& idempotency_key);
    bool mark_manual_review(const std::string& idempotency_key, const std::string& error_code);
    bool fail(const std::string& idempotency_key, const std::string& error_code);
    bool cancel(const std::string& idempotency_key, const std::string& error_code = "cancelled");
    bool prepare_replay(const std::string& idempotency_key, std::size_t attempt);

    std::optional<ToolEffectRecord> find_idempotency(const std::string& idempotency_key) const;
    std::vector<ToolEffectRecord> recoverable() const;
    ToolReconciliationAction reconciliation_action(const ToolEffectRecord& record) const noexcept;
    bool durable() const noexcept { return wal_path_.has_value(); }
    std::size_t size() const;

private:
    bool transition(const std::string& key, ToolEffectStatus expected,
                    ToolEffectStatus next, const std::string& detail);
    void append_locked(ToolEffectRecord& record);
    void load_locked();

    mutable std::mutex mutex_;
    std::map<std::string, ToolEffectRecord> by_key_;
    std::optional<std::filesystem::path> wal_path_;
    std::uint64_t next_sequence_{1};
};

} // namespace agent_framework
#endif
