#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>

#include "agent/internal/platform_io.hpp"
#include "agent/memory_v2/project_instruction_provider.hpp"
#include "agent/memory_v2/view_engine.hpp"
#include "agent/memory_v2/view_profiles.hpp"

int main() {
    using namespace agent_framework;
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("agent-phase4-instructions-" + std::to_string(internal::current_process_id()));
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root / "src" / "module");
    {
        std::ofstream(root / "AGENTS.md") << "root rule";
        std::ofstream(root / "src" / "AGENTS.md") << "ignored normal rule";
        std::ofstream(root / "src" / "AGENTS.override.md") << "override rule";
        std::ofstream(root / "src" / "module" / "AGENTS.md") << "module rule";
    }

    memory_v2::ProjectInstructionProviderConfig config;
    config.tenant_id = "tenant-a";
    config.project_id = "project-a";
    config.workspace_id = "workspace-a";
    config.workspace_root = root;
    config.working_directory = root / "src" / "module";
    auto provider = std::make_shared<memory_v2::ProjectInstructionProvider>(config);

    memory_v2::MemoryScope subject;
    subject.tenant_id = "tenant-a";
    subject.project_id = "project-a";
    subject.workspace_id = "workspace-a";
    subject.path_scope = fs::weakly_canonical(root / "src" / "module").generic_string();
    subject.task_id = "task-a";
    memory_v2::MemoryQuery query{subject, "user-a", {}, 100};
    const auto records = provider->fetch(query);
    assert(records.error.empty());
    assert(records.records.size() == 3);
    assert(records.records[0].content.at("text") == "root rule");
    assert(records.records[1].content.at("text") == "override rule");
    assert(records.records[2].content.at("text") == "module rule");

    memory_v2::MemoryProviderRegistry registry;
    assert(registry.register_provider(provider));
    contracts::ContractMetadata metadata;
    metadata.identity.tenant_id = "tenant-a";
    metadata.identity.principal_id = "user-a";
    metadata.identity.project_id = "project-a";
    metadata.identity.task_id = "task-a";
    metadata.extensions["policy_revision"] = "policy-v1";
    auto spec = memory_v2::make_view_spec(memory_v2::MemoryViewMode::Planning,
                                           metadata, subject);
    memory_v2::MemoryViewEngine engine(registry);
    const auto view = engine.build(spec);
    assert(!view.fail_closed);
    assert(view.records.size() == 3);
    assert(view.records.front().content.at("text") == "module rule");

    memory_v2::MemoryViewRouter router;
    assert(router.route(memory_v2::MemoryViewMode::Planning, "plan_approved") ==
           memory_v2::MemoryViewMode::Execution);
    assert(router.route(memory_v2::MemoryViewMode::Execution, "verification_started") ==
           memory_v2::MemoryViewMode::Verification);
    assert(router.route(memory_v2::MemoryViewMode::Verification, "evaluation_started") ==
           memory_v2::MemoryViewMode::Evaluation);
    assert(!router.route(memory_v2::MemoryViewMode::Execution, "unknown_event"));

    auto wrong_tenant = query;
    wrong_tenant.subject.tenant_id = "tenant-b";
    assert(provider->fetch(wrong_tenant).records.empty());
    fs::remove_all(root, error);
    return 0;
}
