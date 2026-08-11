#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/harness/types.hpp"

namespace agent_framework::harness {

enum class HarnessStoreStatus {
    Committed,
    AlreadyExists,
    NotFound,
    RevisionConflict,
    Invalid,
    Busy,
    Error
};

struct HarnessStoreCommit {
    HarnessStoreStatus status{HarnessStoreStatus::Error};
    std::uint64_t revision{0};
    std::string digest;
    std::string error;
    explicit operator bool() const noexcept {
        return status == HarnessStoreStatus::Committed;
    }
};

struct StoredHarnessCheckpoint {
    HarnessCheckpoint checkpoint;
    std::uint64_t revision{0};
    std::string digest;
};

struct HarnessEvent {
    std::string harness_id;
    std::uint64_t sequence{0};
    std::uint64_t checkpoint_revision{0};
    std::string event_type;
    nlohmann::json payload = nlohmann::json::object();
    std::string payload_digest;
    std::string created_at;
};

class HarnessStore {
public:
    virtual ~HarnessStore() = default;
    virtual HarnessStoreCommit create(const HarnessCheckpoint& checkpoint,
                                      const HarnessEvent& event) = 0;
    virtual std::optional<StoredHarnessCheckpoint> load(
        std::string_view tenant_id, std::string_view harness_id) = 0;
    virtual HarnessStoreCommit compare_exchange(
        const HarnessCheckpoint& checkpoint, std::uint64_t expected_revision,
        const HarnessEvent& event) = 0;
    virtual std::vector<HarnessEvent> events(
        std::string_view tenant_id, std::string_view harness_id,
        std::uint64_t after_sequence = 0) = 0;
    virtual std::vector<StoredHarnessCheckpoint> list_recoverable(
        std::string_view tenant_id, std::size_t limit) = 0;
};

class InMemoryHarnessStore final : public HarnessStore {
public:
    HarnessStoreCommit create(const HarnessCheckpoint& checkpoint,
                              const HarnessEvent& event) override;
    std::optional<StoredHarnessCheckpoint> load(
        std::string_view tenant_id, std::string_view harness_id) override;
    HarnessStoreCommit compare_exchange(
        const HarnessCheckpoint& checkpoint, std::uint64_t expected_revision,
        const HarnessEvent& event) override;
    std::vector<HarnessEvent> events(
        std::string_view tenant_id, std::string_view harness_id,
        std::uint64_t after_sequence = 0) override;
    std::vector<StoredHarnessCheckpoint> list_recoverable(
        std::string_view tenant_id, std::size_t limit) override;

private:
    static std::string key(std::string_view tenant_id, std::string_view harness_id);
    mutable std::mutex mutex_;
    std::map<std::string, StoredHarnessCheckpoint> checkpoints_;
    std::map<std::string, std::vector<HarnessEvent>> events_;
};

struct SQLiteHarnessStoreOptions {
    int busy_timeout_ms{3000};
    bool require_private_permissions{true};
};

class SQLiteHarnessStore final : public HarnessStore {
public:
    explicit SQLiteHarnessStore(std::string path,
                                SQLiteHarnessStoreOptions options = {});
    ~SQLiteHarnessStore() override;
    SQLiteHarnessStore(const SQLiteHarnessStore&) = delete;
    SQLiteHarnessStore& operator=(const SQLiteHarnessStore&) = delete;

    HarnessStoreCommit create(const HarnessCheckpoint& checkpoint,
                              const HarnessEvent& event) override;
    std::optional<StoredHarnessCheckpoint> load(
        std::string_view tenant_id, std::string_view harness_id) override;
    HarnessStoreCommit compare_exchange(
        const HarnessCheckpoint& checkpoint, std::uint64_t expected_revision,
        const HarnessEvent& event) override;
    std::vector<HarnessEvent> events(
        std::string_view tenant_id, std::string_view harness_id,
        std::uint64_t after_sequence = 0) override;
    std::vector<StoredHarnessCheckpoint> list_recoverable(
        std::string_view tenant_id, std::size_t limit) override;

    const std::string& path() const noexcept { return path_; }

private:
    void migrate();
    void* db_{nullptr};
    std::string path_;
    SQLiteHarnessStoreOptions options_;
    std::mutex mutex_;
};

}  // namespace agent_framework::harness
