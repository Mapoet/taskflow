#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/run/types.hpp"

namespace agent_framework::run {

enum class StoreStatus { Committed, AlreadyExists, NotFound, RevisionConflict, Busy, Invalid, Error };

struct StoreResult {
    StoreStatus status{StoreStatus::Error};
    std::uint64_t revision{0};
    std::string error;
    explicit operator bool() const noexcept { return status == StoreStatus::Committed; }
};

struct RunRecord {
    RunCheckpoint checkpoint;
    std::uint64_t revision{0};
    std::string updated_at;
};

struct RunEvent {
    std::string run_id;
    std::uint64_t sequence{0};
    std::string event_type;
    nlohmann::json payload = nlohmann::json::object();
    std::string payload_digest;
    std::string created_at;
    std::string previous_digest;
    std::string event_digest;
    std::string state_digest;
};

enum class EffectState { Prepared, Unknown, Committed, Reconciled };

struct EffectRecord {
    std::string effect_id;
    std::string idempotency_key;
    EffectState state{EffectState::Prepared};
    std::string request_digest;
    std::string receipt_digest;
    std::uint64_t fencing_token{0};
};

struct RunCommit {
    RunCheckpoint checkpoint;
    std::uint64_t expected_revision{0};
    std::string event_type;
    nlohmann::json event_payload = nlohmann::json::object();
    std::optional<EffectRecord> effect;
    std::optional<Interruption> interruption;
};

struct HistoricalRun {
    RunCheckpoint checkpoint;
    std::uint64_t sequence{0};
    std::string state_digest;
};

struct HistoryVerification {
    bool valid{false};
    std::uint64_t verified_events{0};
    std::string error;
};

struct GraphDefinitionRef {
    std::string template_id;
    std::string revision;
    std::string definition_digest;
    std::string compatibility_class;
};

struct DurableTimer {
    std::string timer_id;
    std::string run_id;
    std::int64_t due_unix_ms{0};
    nlohmann::json payload = nlohmann::json::object();
    std::string owner;
    std::int64_t lease_until_unix_ms{0};
    bool completed{false};
};

class RunStore {
public:
    virtual ~RunStore() = default;
    virtual StoreResult create(const RunCheckpoint& initial) = 0;
    virtual std::optional<RunRecord> load(std::string_view run_id) = 0;
    virtual StoreResult checkpoint(const RunCheckpoint& next,
                                   std::uint64_t expected_revision) = 0;
    virtual std::vector<RunRecord> list_recoverable(std::size_t limit) = 0;
    virtual StoreResult append_event(const RunEvent& event) = 0;
    virtual std::vector<RunEvent> events(std::string_view run_id,
                                        std::uint64_t after_sequence = 0) = 0;
    virtual StoreResult put_interruption(const Interruption& interruption) = 0;
    virtual std::optional<Interruption> load_interruption(std::string_view interruption_id) = 0;
    virtual StoreResult consume_resume_token(std::string_view interruption_id,
                                             std::string_view run_id,
                                             std::string_view token_digest) = 0;
    virtual StoreResult register_graph(const GraphDefinitionRef& graph) = 0;
    virtual std::optional<GraphDefinitionRef> load_graph(std::string_view template_id,
                                                        std::string_view revision) = 0;
    virtual StoreResult schedule_timer(const DurableTimer& timer) = 0;
    virtual std::vector<DurableTimer> claim_due_timers(std::int64_t now_unix_ms,
                                                       std::string_view owner,
                                                       std::int64_t lease_ms,
                                                       std::size_t limit) = 0;
    virtual StoreResult complete_timer(std::string_view timer_id,
                                       std::string_view owner) = 0;
    virtual StoreResult commit(const RunCommit& commit) = 0;
    virtual std::optional<HistoricalRun> reconstruct(std::string_view run_id,
                                                     std::uint64_t sequence) = 0;
    virtual HistoryVerification verify_history(std::string_view run_id) = 0;
    virtual std::optional<EffectRecord> effect(std::string_view run_id,
                                               std::string_view effect_id) = 0;
};

struct SQLiteRunStoreOptions {
    int busy_timeout_ms{3000};
    bool require_private_permissions{true};
};

class SQLiteRunStore final : public RunStore {
public:
    explicit SQLiteRunStore(std::string path, SQLiteRunStoreOptions options = {});
    ~SQLiteRunStore() override;
    SQLiteRunStore(const SQLiteRunStore&) = delete;
    SQLiteRunStore& operator=(const SQLiteRunStore&) = delete;

    StoreResult create(const RunCheckpoint& initial) override;
    std::optional<RunRecord> load(std::string_view run_id) override;
    StoreResult checkpoint(const RunCheckpoint& next, std::uint64_t expected_revision) override;
    std::vector<RunRecord> list_recoverable(std::size_t limit) override;
    StoreResult append_event(const RunEvent& event) override;
    std::vector<RunEvent> events(std::string_view run_id,
                                 std::uint64_t after_sequence = 0) override;
    StoreResult put_interruption(const Interruption& interruption) override;
    std::optional<Interruption> load_interruption(std::string_view interruption_id) override;
    StoreResult consume_resume_token(std::string_view interruption_id, std::string_view run_id,
                                     std::string_view token_digest) override;
    StoreResult register_graph(const GraphDefinitionRef& graph) override;
    std::optional<GraphDefinitionRef> load_graph(std::string_view template_id,
                                                std::string_view revision) override;
    StoreResult schedule_timer(const DurableTimer& timer) override;
    std::vector<DurableTimer> claim_due_timers(std::int64_t now_unix_ms,
                                               std::string_view owner,
                                               std::int64_t lease_ms,
                                               std::size_t limit) override;
    StoreResult complete_timer(std::string_view timer_id, std::string_view owner) override;
    StoreResult commit(const RunCommit& commit) override;
    std::optional<HistoricalRun> reconstruct(std::string_view run_id,
                                             std::uint64_t sequence) override;
    HistoryVerification verify_history(std::string_view run_id) override;
    std::optional<EffectRecord> effect(std::string_view run_id,
                                       std::string_view effect_id) override;

    const std::string& path() const noexcept { return path_; }

private:
    void migrate();
    void* db_{nullptr};
    std::string path_;
    SQLiteRunStoreOptions options_;
    std::mutex mutex_;
};

}  // namespace agent_framework::run
