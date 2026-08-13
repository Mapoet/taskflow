#include <agent/agent_template/registry.hpp>

#include <cassert>
#include <filesystem>
#include <iostream>

using namespace agent_framework;
using namespace agent_framework::agent_template;
namespace fs = std::filesystem;

contracts::ContractMetadata meta()
{
    contracts::ContractMetadata m;
    m.identity.tenant_id = "tenant";
    m.identity.task_id = "task";
    return m;
}
AgentTemplate templ(std::uint64_t revision)
{
    AgentTemplate t;
    t.metadata = meta();
    t.template_id = "research";
    t.revision = revision;
    t.name = "Research";
    t.permissions.tools = {"web_search"};
    return t;
}

int main()
{
    const fs::path path = fs::temp_directory_path() / "agent_template_registry.sqlite3";
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove(path.string() + "-wal", ec);
    fs::remove(path.string() + "-shm", ec);
    std::string template_digest;
    {
        SQLiteAgentTemplateRegistry registry(path.string());
        auto t = templ(1);
        auto result = registry.publish(t);
        assert(result.status == RegistryStatus::Committed);
        template_digest = result.digest;
        assert(registry.publish(t).status == RegistryStatus::AlreadyExists);
        auto conflict = t;
        conflict.name = "changed same revision";
        assert(registry.publish(conflict).status == RegistryStatus::RevisionConflict);
        auto second = templ(2);
        second.name = "Research v2";
        assert(registry.publish(second).ok());
        assert(registry.latest("tenant", "research")->revision == 2);

        AgentTemplateInvocation invocation;
        invocation.metadata = meta();
        invocation.invocation_id = "inv-1";
        invocation.template_ref = {"research", 1, template_digest};
        invocation.plan_digest = "sha256:plan";
        invocation.skill_session_digest = "sha256:session";
        invocation.model_profiles_digest = "sha256:model";
        invocation.capability_snapshot_digest = "sha256:caps";
        invocation.deployment_generation = "deploy-1";
        assert(registry.create_invocation(invocation).status == RegistryStatus::Committed);
        invocation.context_projection_ref = "cas://context";
        assert(registry.update_invocation(invocation, 1).revision == 2);
        assert(registry.update_invocation(invocation, 1).status == RegistryStatus::RevisionConflict);
    }
    {
        SQLiteAgentTemplateRegistry restarted(path.string());
        auto first = restarted.load("tenant", "research", 1);
        assert(first && encode(*first).at("canonical_digest") == template_digest);
        auto invocation = restarted.load_invocation("tenant", "inv-1");
        assert(invocation && invocation->store_revision == 2);
        assert(invocation->invocation.template_ref.revision == 1);
        assert(invocation->invocation.deployment_generation == "deploy-1");
        auto incomplete = invocation->invocation;
        incomplete.invocation_id = "inv-bad";
        incomplete.plan_digest.clear();
        assert(restarted.create_invocation(incomplete).status == RegistryStatus::Invalid);
    }
    fs::remove(path, ec);
    fs::remove(path.string() + "-wal", ec);
    fs::remove(path.string() + "-shm", ec);
    std::cout << "test_agent_template_registry: ok\n";
}
