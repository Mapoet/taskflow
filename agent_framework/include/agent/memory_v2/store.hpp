#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/memory_v2/types.hpp"

namespace agent_framework::memory_v2 {

enum class CommitStatus { Committed, AlreadyExists, NotFound, RevisionConflict, Forbidden, Invalid, Busy, Error };
struct CommitResult {
    CommitStatus status{CommitStatus::Error};
    std::uint64_t revision{0};
    std::string error;
    explicit operator bool() const noexcept { return status == CommitStatus::Committed; }
};

struct MemoryQuery {
    MemoryScope subject;
    std::string principal_id;
    std::vector<MemoryLevel> levels;
    std::size_t limit{1000};
};

bool can_transition(MemoryStatus from, MemoryStatus to) noexcept;
bool memory_visible_to(const MemoryRecord& record, const MemoryQuery& query) noexcept;

class MemoryStore {
public:
    virtual ~MemoryStore() = default;
    virtual CommitResult append(const MemoryRecord& record,
                                std::string_view approval_id = {}) = 0;
    virtual CommitResult revise(const MemoryRecord& record, std::uint64_t expected_revision,
                                std::string_view approval_id = {}) = 0;
    virtual std::optional<MemoryRecord> current(std::string_view record_id) = 0;
    virtual std::vector<MemoryRecord> history(std::string_view record_id) = 0;
    virtual std::vector<MemoryRecord> query(const MemoryQuery& query) = 0;
    virtual std::uint64_t generation() = 0;
};

struct SQLiteMemoryStoreOptions {
    int busy_timeout_ms{3000};
    bool require_private_permissions{true};
};

class SQLiteMemoryStore final : public MemoryStore {
public:
    explicit SQLiteMemoryStore(std::string path, SQLiteMemoryStoreOptions options = {});
    ~SQLiteMemoryStore() override;
    SQLiteMemoryStore(const SQLiteMemoryStore&) = delete;
    SQLiteMemoryStore& operator=(const SQLiteMemoryStore&) = delete;
    CommitResult append(const MemoryRecord& record, std::string_view approval_id = {}) override;
    CommitResult revise(const MemoryRecord& record, std::uint64_t expected_revision,
                        std::string_view approval_id = {}) override;
    std::optional<MemoryRecord> current(std::string_view record_id) override;
    std::vector<MemoryRecord> history(std::string_view record_id) override;
    std::vector<MemoryRecord> query(const MemoryQuery& query) override;
    std::uint64_t generation() override;
private:
    void migrate();
    void* db_{nullptr};
    std::string path_;
    SQLiteMemoryStoreOptions options_;
    std::mutex mutex_;
};

}  // namespace agent_framework::memory_v2
