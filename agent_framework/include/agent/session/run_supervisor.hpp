#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace agent_framework::session {

enum class SupervisedRunState { Queued, Leased, Running, AwaitingInput, Completed, Failed, Cancelled };
enum class SessionCommandKind {
    Start, Steer, Queue, Comment, Fork, Cancel, Retry, Reconcile, Escalate
};

struct SessionRunRequest {
    std::string tenant_id, organization_id, project_id, principal_id, provider_id;
    std::string session_id, run_id, command_id;
    nlohmann::json payload=nlohmann::json::object();
    std::string created_at;
};

struct SessionRunCommand {
    std::string tenant_id, session_id, run_id, command_id;
    SessionCommandKind kind{SessionCommandKind::Queue};
    nlohmann::json payload=nlohmann::json::object();
    std::uint64_t sequence{0};
    std::string created_at;
    // Required by versioned production mutations. Kept at the end so the
    // legacy aggregate wire adapter remains source compatible.
    std::optional<std::uint64_t> expected_run_revision;
};

struct SupervisedRun {
    SessionRunRequest request;
    SupervisedRunState state{SupervisedRunState::Queued};
    std::uint64_t revision{1}, lease_epoch{0};
    std::uint64_t command_cursor{0};
    std::string lease_owner;
    std::uint64_t lease_expires_at_ms{0};
    std::string updated_at;
};

struct RunSupervisorQuota {
    std::size_t organization_active{32};
    std::size_t project_active{16};
    std::size_t principal_active{8};
    std::size_t provider_active{16};
};

struct RunSupervisorResult { bool ok{false};std::uint64_t revision{0},lease_epoch{0};std::string error; };

std::string_view name(SupervisedRunState);
std::string_view name(SessionCommandKind);

class SQLiteSessionRunSupervisor {
public:
    explicit SQLiteSessionRunSupervisor(std::string database_path,
                                        RunSupervisorQuota quota={});
    ~SQLiteSessionRunSupervisor();
    SQLiteSessionRunSupervisor(const SQLiteSessionRunSupervisor&)=delete;
    SQLiteSessionRunSupervisor& operator=(const SQLiteSessionRunSupervisor&)=delete;

    RunSupervisorResult enqueue(SessionRunRequest);
    RunSupervisorResult enqueue_command(SessionRunCommand);
    std::optional<SupervisedRun> claim_next(std::string_view worker_id,
        std::uint64_t now_ms,std::uint64_t lease_ms,std::string* error=nullptr);
    RunSupervisorResult mark_running(std::string_view tenant,std::string_view run_id,
        std::string_view worker,std::uint64_t lease_epoch,std::uint64_t expected_revision);
    RunSupervisorResult renew(std::string_view tenant,std::string_view run_id,
        std::string_view worker,std::uint64_t lease_epoch,std::uint64_t expected_revision,
        std::uint64_t expires_at_ms);
    RunSupervisorResult await_input(std::string_view tenant,std::string_view run_id,
        std::string_view worker,std::uint64_t lease_epoch,std::uint64_t expected_revision,
        std::uint64_t expires_at_ms,std::uint64_t command_cursor=0);
    RunSupervisorResult finish(std::string_view tenant,std::string_view run_id,
        std::string_view worker,std::uint64_t lease_epoch,std::uint64_t expected_revision,
        SupervisedRunState terminal,std::uint64_t command_cursor=0);
    std::optional<SupervisedRun> load(std::string_view tenant,std::string_view run_id);
    std::vector<SessionRunCommand> commands(std::string_view tenant,
                                            std::string_view run_id,
                                            std::uint64_t after_sequence=0);

private:
    void migrate();void* db_{nullptr};std::string path_;RunSupervisorQuota quota_;std::mutex mutex_;
};

} // namespace agent_framework::session
