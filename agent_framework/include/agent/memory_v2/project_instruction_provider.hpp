#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "agent/memory_v2/provider.hpp"

namespace agent_framework::memory_v2 {

struct ProjectInstructionProviderConfig {
    std::string provider_id{"project-instructions"};
    std::string tenant_id;
    std::string project_id;
    std::string workspace_id;
    std::filesystem::path workspace_root;
    std::filesystem::path working_directory;
    std::optional<std::filesystem::path> global_instruction;
    std::string instruction_name{"AGENTS.md"};
    std::string override_name{"AGENTS.override.md"};
    std::size_t max_document_bytes{256 * 1024};
};

class ProjectInstructionProvider final : public MemoryProvider {
public:
    explicit ProjectInstructionProvider(ProjectInstructionProviderConfig config);
    std::string id() const override;
    ProviderResult fetch(const MemoryQuery& query) override;

private:
    ProjectInstructionProviderConfig config_;
};

}  // namespace agent_framework::memory_v2
