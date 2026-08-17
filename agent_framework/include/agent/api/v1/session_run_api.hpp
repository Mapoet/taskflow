#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "agent/identity/runtime_subject.hpp"
#include "agent/conversation/store.hpp"
#include "agent/approval/store.hpp"
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
                  approval::ApprovalStore* approvals=nullptr)
        : catalog_(catalog), supervisor_(supervisor), events_(events),
          interactions_(interactions), approvals_(approvals) {}

    ApiResult list_sessions(const identity::RuntimeSubject&,
                            session::SessionListQuery) const;
    ApiResult get_session(const identity::RuntimeSubject&,
                          std::string_view session_id) const;
    ApiResult create_session(const identity::RuntimeSubject&,
                             session::ProductSession);
    ApiResult transition_session(const identity::RuntimeSubject&,
        std::string_view session_id, std::uint64_t expected_revision,
        session::ProductSessionState);
    ApiResult enqueue_run(const identity::RuntimeSubject&,
                          session::SessionRunRequest);
    ApiResult enqueue_command(const identity::RuntimeSubject&,
                              session::SessionRunCommand);
    ApiResult get_run(const identity::RuntimeSubject&,
                      std::string_view run_id);
    ApiResult replay_events(const identity::RuntimeSubject&,
        std::string_view session_id,std::uint64_t after,std::size_t limit);
    ApiResult get_artifact(const identity::RuntimeSubject&,
        std::string_view session_id,std::string_view artifact_id);
    ApiResult get_approval(const identity::RuntimeSubject&,
        std::string_view session_id,std::string_view approval_id);
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
};

} // namespace agent_framework::api::v1
