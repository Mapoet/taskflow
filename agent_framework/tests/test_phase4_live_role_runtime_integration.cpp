#include <cassert>
#include <memory>

#include "phase4_live_test_support.hpp"
#include "phase4_llm_runtime_test_support.hpp"
#include "agent/prompt_renderer/prompt_renderer.hpp"

int main()
{
    using namespace phase4_live_test;
    using namespace phase4_llm_test;
    using namespace agent_framework;
    using namespace agent_framework::live;
    using namespace agent_framework::llm_runtime;
    auto runtime_store = std::make_shared<InMemoryLLMRuntimeStore>();
    auto e = environment("task-a");
    e.metadata = phase4_llm_test::metadata();
    e.certification_id = "cert-role-runtime";
    e.region = "local";
    RoleLiveMatrix m;
    m.metadata = e.metadata;
    m.matrix_id = "matrix-role-runtime";
    auto add = [&](std::string id, LiveCellKind kind, std::string role, std::string stage, std::vector<std::string> deps = {})
    {LiveCellSpec c;c.cell_id=id;c.kind=kind;c.role=role;c.stage=stage;c.dependencies=std::move(deps);c.profile_id="live."+id;c.profile_revision="profile-r1";c.prompt_id="live.prompt."+id;c.prompt_revision="prompt-r1";c.provider="fake-"+id;c.model="model-"+id;c.independence_group="group-"+id;c.region="local";c.required_capabilities={"repo_read"};if(kind==LiveCellKind::Assurance||kind==LiveCellKind::Judge){c.read_only=true;c.blind=true;}if(kind==LiveCellKind::Assurance)c.strong_oracle_required=true;m.cells.push_back(c);
        auto rp=phase4_llm_test::profile();rp.metadata=e.metadata;rp.profile_id=c.profile_id;rp.role=c.role;rp.prompt_id=c.prompt_id;rp.independence_group=c.independence_group;rp.calibration_revision="cal-"+id;rp.provider_pool={"candidate-"+id};auto pr=phase4_llm_test::prompt();pr.metadata=e.metadata;pr.prompt_id=c.prompt_id;auto cal=phase4_llm_test::calibration(c.provider,c.model);cal.metadata=e.metadata;cal.calibration_id=rp.calibration_revision;cal.role=rp.role;cal.profile_id=rp.profile_id;auto status1=runtime_store->publish_profile(rp);auto status2=runtime_store->publish_prompt(pr);auto status3=runtime_store->publish_calibration(cal);assert(status1.ok()&&status2.ok()&&status3.ok());auto p=profile(c);p.capabilities={"repo_read"};p.region="local";p.calibration_id=cal.calibration_id;p.calibration_digest=llm_runtime::encode(cal).at("canonical_digest");e.role_profiles.push_back(std::move(p)); };
    add("cognition", LiveCellKind::Cognition, "planner", "planning");
    add("memory", LiveCellKind::Memory, "memory-curator", "memory", {"cognition"});
    add("execution", LiveCellKind::Execution, "executor", "execution", {"memory"});
    add("assurance", LiveCellKind::Assurance, "verifier", "assurance", {"execution"});
    add("judge", LiveCellKind::Judge, "judge", "judge", {"assurance"});
    add("recovery", LiveCellKind::FailureRecovery, "recovery", "restart", {"judge"});
    m.cells.back().failure_scenario = LiveFailureScenario::Restart;
    m.environment_digest = role_environment_digest(e);
    assert(validate_live_contract(e, m).empty());
    auto router = std::make_shared<ModelRouter>();
    auto adapter = std::make_shared<ScriptedAdapter>();
    for (int i = 0; i < 6; ++i)
        adapter->push([]
                      { return output(R"({"ok":true})"); });
    auto client = std::make_shared<LLMClient>();
    client->set_prompt_renderer(std::make_shared<PromptRenderer>());
    for (const auto &c : m.cells)
    {
        auto value = candidate("candidate-" + c.cell_id, c.provider, c.model, 10);
        value.independence_group = c.independence_group;
        assert(router->register_candidate(value));
        client->register_adapter(c.provider, adapter);
    }
    auto runtime = std::make_shared<RoleRuntime>(client, runtime_store, router);
    RoleRuntimeLiveCellExecutor executor(runtime);
    for (std::size_t i = 0; i < m.cells.size(); ++i)
    {
        auto req = phase4_llm_test::request("live-inv-" + std::to_string(i));
        req.metadata = e.metadata;
        req.profile_id = m.cells[i].profile_id;
        req.profile_revision = m.cells[i].profile_revision;
        req.required_region = "local";
        RoleRuntimeCellBinding b;
        b.request = std::move(req);
        b.evidence_digests = {"sha256:workflow-evidence-" + std::to_string(i)};
        b.read_only = m.cells[i].read_only;
        b.blind = m.cells[i].blind;
        if (m.cells[i].strong_oracle_required)
            b.oracle_digests = {"sha256:strong-oracle"};
        assert(executor.bind(m.cells[i].cell_id, std::move(b)));
    }
    InMemoryRoleCertificationStore cert_store;
    RoleLiveCertificationWorkflow workflow(cert_store, executor);
    auto r = workflow.run(e, m, phase4_live_test::options("role-runtime-live"));
    assert(r.state == RoleCertificationState::Certified && r.report);
    assert(adapter->calls() == m.cells.size());
    for (const auto &c : r.report->cells)
    {
        assert(c.executed && !c.invocation_manifest_digest.empty());
        assert(c.provider == "fake-" + c.cell_id && c.model == "model-" + c.cell_id);
    }
    return 0;
}
