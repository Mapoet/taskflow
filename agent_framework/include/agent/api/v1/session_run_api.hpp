#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "agent/identity/runtime_subject.hpp"
#include "agent/conversation/store.hpp"
#include "agent/conversation/task_execution_snapshot.hpp"
#include "agent/conversation/task_registry.hpp"
#include "agent/approval/store.hpp"
#include "agent/decision/decision_store.hpp"
#include "agent/ui/interaction_projection_store.hpp"
#include "agent/session/run_supervisor.hpp"
#include "agent/session/session_catalog.hpp"

namespace agent_framework::api::v1 {

struct ApiResult {
    int status{500};
    nlohmann::json body=nlohmann::json::object();
    bool ok() const { return status >= 200 && status < 300; }
};

class SessionRunApi {
public:
    SessionRunApi(session::SessionCatalog& catalog,
                  session::SQLiteSessionRunSupervisor& supervisor,
                  conversation::ConversationStore* events=nullptr,
                  ui::InteractionProjectionStore* interactions=nullptr,
                  approval::ApprovalStore* approvals=nullptr,
                  decision::DecisionStore* decisions=nullptr,
                  conversation::TaskRegistry* tasks=nullptr)
        : catalog_(catalog), supervisor_(supervisor), events_(events),
          interactions_(interactions), approvals_(approvals),decisions_(decisions),tasks_(tasks) {}

    ApiResult list_sessions(const identity::RuntimeSubject&,
                            session::SessionListQuery) const;
    ApiResult get_session(const identity::RuntimeSubject&,
                          std::string_view session_id) const;
    ApiResult create_session(const identity::RuntimeSubject&,
                             session::ProductSession);
    ApiResult fork_session(const identity::RuntimeSubject&,std::string_view source_session_id,
        std::uint64_t expected_source_revision,session::ProductSession forked);
    ApiResult transition_session(const identity::RuntimeSubject&,
        std::string_view session_id, std::uint64_t expected_revision,
        session::ProductSessionState);
    ApiResult restore_session(const identity::RuntimeSubject&,std::string_view session_id,
                              std::uint64_t expected_revision);
    ApiResult purge_session(const identity::RuntimeSubject&,std::string_view session_id,
                            std::uint64_t expected_revision,bool confirm_permanent);
    ApiResult rename_session(const identity::RuntimeSubject&,std::string_view session_id,
                             std::uint64_t expected_revision,std::string_view title);
    ApiResult organize_session(const identity::RuntimeSubject&,std::string_view session_id,
        std::uint64_t expected_revision,std::string_view folder,
        const std::vector<std::string>& tags,bool pinned);
    ApiResult put_session_member(const identity::RuntimeSubject&,std::string_view session_id,
        std::uint64_t expected_member_revision,std::string_view principal_id,
        session::SessionMemberRole member_role);
    ApiResult enqueue_run(const identity::RuntimeSubject&,
                          session::SessionRunRequest);
    ApiResult enqueue_command(const identity::RuntimeSubject&,
                              session::SessionRunCommand);
    ApiResult get_run(const identity::RuntimeSubject&,
                      std::string_view run_id);
    ApiResult replay_events(const identity::RuntimeSubject&,
        std::string_view session_id,std::uint64_t after,std::size_t limit);
    ApiResult get_session_data(const identity::RuntimeSubject&,
        std::string_view session_id,std::uint64_t after,std::size_t limit);
    ApiResult get_artifact(const identity::RuntimeSubject&,
        std::string_view session_id,std::string_view artifact_id);
    ApiResult get_interactions(const identity::RuntimeSubject&,
        std::string_view session_id,ui::InteractionVisibility viewer=ui::InteractionVisibility::User);
    ApiResult get_approval(const identity::RuntimeSubject&,
        std::string_view session_id,std::string_view approval_id);
    ApiResult get_decision(const identity::RuntimeSubject&,
        std::string_view session_id,std::string_view decision_id);
    ApiResult answer_decision(const identity::RuntimeSubject&,
        std::string_view session_id,std::string_view decision_id,
        std::uint64_t expected_revision,std::string_view option_id,
        std::uint64_t now_ms);
    ApiResult get_execution_snapshot(const identity::RuntimeSubject&,
        std::string_view session_id,std::string_view task_id,
        std::string_view run_id={});
    ApiResult get_task(const identity::RuntimeSubject&,std::string_view session_id,
                       std::string_view task_id);
    ApiResult capabilities(const identity::RuntimeSubject&,
                           std::string_view session_id);

private:
    std::optional<session::SessionMemberRole> role(
        const identity::RuntimeSubject&, std::string_view session_id) const;
    static ApiResult denied(std::string code, int status=403);
    session::SessionCatalog& catalog_;
    session::SQLiteSessionRunSupervisor& supervisor_;
    conversation::ConversationStore* events_{nullptr};
    ui::InteractionProjectionStore* interactions_{nullptr};
    approval::ApprovalStore* approvals_{nullptr};
    decision::DecisionStore* decisions_{nullptr};
    conversation::TaskRegistry* tasks_{nullptr};
};

} // namespace agent_framework::api::v1
