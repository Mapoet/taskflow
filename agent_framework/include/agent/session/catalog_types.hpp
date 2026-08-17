#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agent_framework::session {

enum class ProductSessionState { Active, Archived, Trashed, PurgePending, Purged };
enum class SessionMemberRole { Viewer, Contributor, Operator, Owner };

struct ProductSession {
    std::string tenant_id;
    std::string organization_id;
    std::string project_id;
    std::string workspace_id;
    std::string session_id;
    std::string conversation_id;
    std::string owner_principal_id;
    std::string title;
    std::string folder;
    std::vector<std::string> tags;
    bool pinned{false};
    ProductSessionState state{ProductSessionState::Active};
    std::uint64_t revision{1};
    std::uint64_t sequence{0};
    std::string created_at;
    std::string updated_at;
};

struct SessionMember {
    std::string tenant_id;
    std::string session_id;
    std::string principal_id;
    SessionMemberRole role{SessionMemberRole::Viewer};
    std::uint64_t revision{1};
};

struct SessionListQuery {
    std::string tenant_id;
    std::string principal_id;
    std::string search;
    std::optional<ProductSessionState> state{ProductSessionState::Active};
    std::uint64_t before_sequence{0};
    std::size_t limit{50};
    std::string organization_id;
    std::string project_id;
    std::string workspace_id;
};

struct SessionListPage {
    std::vector<ProductSession> sessions;
    std::uint64_t next_before_sequence{0};
};

struct SessionMutationResult {
    bool ok{false};
    std::uint64_t revision{0};
    std::string error;
};

std::string_view name(ProductSessionState);
std::string_view name(SessionMemberRole);

}  // namespace agent_framework::session
