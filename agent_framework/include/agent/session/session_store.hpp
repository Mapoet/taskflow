#ifndef AGENT_FRAMEWORK_SESSION_STORE_HPP
#define AGENT_FRAMEWORK_SESSION_STORE_HPP

#include <agent/internal/agent_thread_state.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace agent_framework {

struct ToolCommitRecord {
    std::string tool_call_id;
    std::size_t attempt = 0;
    std::string status;
    std::string result_digest;
};

struct ChildTaskSnapshot {
    std::string child_id;
    std::string backend;
    std::size_t attempt = 0;
    std::string status;
    json payload = json::object();
};

struct SessionSnapshot {
    std::string session_id;
    std::uint64_t revision = 0;
    std::string checkpoint_id;
    internal::AgentThreadState state;
    std::vector<ToolCommitRecord> tool_commits;
    std::vector<ChildTaskSnapshot> child_tasks;
};

enum class SessionCommitStatus { Committed, RevisionConflict, StoreBusy, Error };

struct SessionCommitResult {
    SessionCommitStatus status = SessionCommitStatus::Error;
    std::uint64_t revision = 0;
    std::string error;
};

class SessionPayloadCodec {
public:
    virtual ~SessionPayloadCodec() = default;
    virtual std::string encode(std::string_view plain) const = 0;
    virtual std::string decode(std::string_view encoded) const = 0;
};

class IdentitySessionPayloadCodec final : public SessionPayloadCodec {
public:
    std::string encode(std::string_view plain) const override { return std::string(plain); }
    std::string decode(std::string_view encoded) const override { return std::string(encoded); }
};

class SessionStore {
public:
    virtual ~SessionStore() = default;
    // Side-effect-free current-state lookup for projections, diagnostics and
    // recovery. Observers must never create a session merely by reading it.
    virtual std::optional<SessionSnapshot> load_current(std::string_view session_id) = 0;
    virtual SessionSnapshot load_or_create(std::string_view session_id) = 0;
    virtual SessionCommitResult commit(const SessionSnapshot& next,
                                       std::uint64_t expected_revision) = 0;
    virtual std::optional<SessionSnapshot> load_checkpoint(
        std::string_view session_id, std::string_view checkpoint_id) = 0;
};

class InMemorySessionStore final : public SessionStore {
public:
    std::optional<SessionSnapshot> load_current(std::string_view session_id) override;
    SessionSnapshot load_or_create(std::string_view session_id) override;
    SessionCommitResult commit(const SessionSnapshot& next,
                               std::uint64_t expected_revision) override;
    std::optional<SessionSnapshot> load_checkpoint(
        std::string_view session_id, std::string_view checkpoint_id) override;

private:
    std::mutex mutex_;
    std::unordered_map<std::string, SessionSnapshot> sessions_;
};

struct SQLiteSessionStoreOptions {
    int busy_timeout_ms = 2000;
    bool require_private_permissions = true;
    std::shared_ptr<SessionPayloadCodec> codec =
        std::make_shared<IdentitySessionPayloadCodec>();
};

class SQLiteSessionStore final : public SessionStore {
public:
    explicit SQLiteSessionStore(std::string path, SQLiteSessionStoreOptions options = {});
    ~SQLiteSessionStore() override;
    SQLiteSessionStore(const SQLiteSessionStore&) = delete;
    SQLiteSessionStore& operator=(const SQLiteSessionStore&) = delete;

    std::optional<SessionSnapshot> load_current(std::string_view session_id) override;
    SessionSnapshot load_or_create(std::string_view session_id) override;
    SessionCommitResult commit(const SessionSnapshot& next,
                               std::uint64_t expected_revision) override;
    std::optional<SessionSnapshot> load_checkpoint(
        std::string_view session_id, std::string_view checkpoint_id) override;

    const std::string& path() const { return path_; }

private:
    void* db_ = nullptr;
    std::string path_;
    SQLiteSessionStoreOptions options_;
    std::mutex mutex_;
    void migrate();
};

json agent_thread_state_to_json(const internal::AgentThreadState& state);
internal::AgentThreadState agent_thread_state_from_json(const json& value);

}  // namespace agent_framework

#endif
