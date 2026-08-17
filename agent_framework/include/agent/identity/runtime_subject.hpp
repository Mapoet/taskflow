#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "agent/conversation/types.hpp"

namespace agent_framework::identity {

enum class SubjectBoundary { LocalLegacy, Internal, Production };

struct RuntimeSubject {
    std::string tenant_id;
    std::string organization_id;
    std::string principal_id;
    std::string project_id;
    std::string workspace_id;
    std::string session_id;
    std::string conversation_id;
    std::string task_id;
    std::string run_id;
    std::string turn_id;
    std::string agent_id;
    std::uint64_t authorization_revision{0};
    bool authenticated{false};
    bool legacy_adapted{false};
};

std::vector<std::string> validate(const RuntimeSubject&, SubjectBoundary);
conversation::ConversationIdentity conversation_identity(const RuntimeSubject&);
RuntimeSubject legacy_local_subject(std::string_view session_id,
                                    std::string_view conversation_id);

}  // namespace agent_framework::identity
