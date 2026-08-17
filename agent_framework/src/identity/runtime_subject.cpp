#include "agent/identity/runtime_subject.hpp"

namespace agent_framework::identity {

std::vector<std::string> validate(const RuntimeSubject& value,
                                  SubjectBoundary boundary) {
    std::vector<std::string> errors;
    if(value.tenant_id.empty() || value.session_id.empty() ||
       value.conversation_id.empty())
        errors.push_back("identity_required");
    if(value.run_id.empty() != value.task_id.empty())
        errors.push_back("identity_inconsistent:task_run_pair");
    if(!value.turn_id.empty() && value.conversation_id.empty())
        errors.push_back("identity_inconsistent:turn_without_conversation");
    if(boundary == SubjectBoundary::Production) {
        if(value.legacy_adapted) errors.push_back("legacy_identity_forbidden");
        if(!value.authenticated || value.principal_id.empty())
            errors.push_back("authentication_required");
        if(value.organization_id.empty() || value.project_id.empty() ||
           value.workspace_id.empty() || value.agent_id.empty())
            errors.push_back("identity_required:production_scope");
        if(value.authorization_revision == 0)
            errors.push_back("authorization_revision_required");
    }
    return errors;
}

conversation::ConversationIdentity conversation_identity(
    const RuntimeSubject& value) {
    return {value.tenant_id, value.conversation_id};
}

RuntimeSubject legacy_local_subject(std::string_view session,
                                    std::string_view conversation) {
    RuntimeSubject value;
    value.tenant_id = "local";
    value.organization_id = "local";
    value.principal_id = "local-user";
    value.project_id = "local";
    value.workspace_id = "local";
    value.session_id = std::string(session);
    value.conversation_id = std::string(conversation);
    value.agent_id = "local-agent";
    value.authorization_revision = 1;
    value.authenticated = true;
    value.legacy_adapted = true;
    return value;
}

}  // namespace agent_framework::identity
