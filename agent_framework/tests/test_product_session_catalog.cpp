#include <cassert>
#include <filesystem>

#include "agent/session/session_catalog.hpp"

using namespace agent_framework::session;

ProductSession make_session(std::string id, std::string tenant = "tenant")
{
    ProductSession value;
    value.tenant_id = std::move(tenant);
    value.organization_id = "org";
    value.project_id = "project";
    value.workspace_id = "workspace";
    value.session_id = id;
    value.conversation_id = "conversation-" + id;
    value.owner_principal_id = "owner";
    value.title = "Session " + id;
    return value;
}
int main()
{
    const auto path = (std::filesystem::temp_directory_path() / "agent-product-session-test.sqlite").string();
    std::filesystem::remove(path);
    {
        SQLiteSessionCatalog catalog(path);
        assert(catalog.create(make_session("s1")).ok);
        assert(catalog.create(make_session("s1")).ok);
        assert(catalog.create(make_session("s2")).ok);
        assert(catalog.create(make_session("s3", "other")).ok);
        SessionListQuery query{"tenant", "owner", "", ProductSessionState::Active, 0, 1};
        auto first = catalog.list(query);
        assert(first.sessions.size() == 1 && first.next_before_sequence > 0);
        query.before_sequence = first.next_before_sequence;
        auto second = catalog.list(query);
        assert(second.sessions.size() == 1 && second.sessions[0].session_id != first.sessions[0].session_id);
        auto renamed = catalog.rename("tenant", "s1", 1, "Renamed");
        assert(renamed.ok && renamed.revision == 2);
        assert(!catalog.rename("tenant", "s1", 1, "Stale").ok);
        auto organized = catalog.organize("tenant", "s1", 2, "science", {"gnss", "weather"}, true);
        assert(organized.ok);
        assert(catalog.transition("tenant", "s1", 3, ProductSessionState::Archived).ok);
        assert(!catalog.transition("tenant", "s1", 4, ProductSessionState::PurgePending).ok);
        assert(catalog.transition("tenant", "s1", 4, ProductSessionState::Trashed).ok);
        assert(catalog.transition("tenant", "s1", 5, ProductSessionState::PurgePending).ok);
        SessionMember member{"tenant", "s2", "viewer", SessionMemberRole::Viewer};
        assert(catalog.put_member(member, 0).ok);
        SessionListQuery visible{"tenant", "viewer"};
        assert(catalog.list(visible).sessions.size() == 1);
        SessionListQuery cross{"other", "viewer"};
        assert(catalog.list(cross).sessions.empty());
    }
    {
        SQLiteSessionCatalog reopened(path);
        auto recovered = reopened.get("tenant", "s1");
        assert(recovered && recovered->state == ProductSessionState::PurgePending && recovered->title == "Renamed" && recovered->pinned);
    }
    std::filesystem::remove(path);
}
