#include <cassert>

#include "agent/identity/runtime_subject.hpp"

using namespace agent_framework::identity;

int main() {
    auto legacy = legacy_local_subject("session", "conversation");
    assert(validate(legacy, SubjectBoundary::LocalLegacy).empty());
    assert(!validate(legacy, SubjectBoundary::Production).empty());

    RuntimeSubject production;
    production.tenant_id = "tenant";
    production.organization_id = "organization";
    production.principal_id = "principal";
    production.project_id = "project";
    production.workspace_id = "workspace";
    production.session_id = "session";
    production.conversation_id = "conversation";
    production.agent_id = "agent";
    production.authorization_revision = 7;
    production.authenticated = true;
    assert(validate(production, SubjectBoundary::Production).empty());
    production.run_id = "run-without-task";
    assert(!validate(production, SubjectBoundary::Internal).empty());
}
