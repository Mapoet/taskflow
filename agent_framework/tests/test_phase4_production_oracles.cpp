#include <cassert>
#include <filesystem>
#include <fstream>

#include "agent/assurance/production_oracles.hpp"
#include "agent/internal/platform_io.hpp"
#include "agent/sandbox/process_provider.hpp"
#include "phase4_assurance_workflow_test_support.hpp"

int main() {
    using namespace agent_framework;
    using namespace agent_framework::assurance;
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("phase4-production-oracles-"+
        std::to_string(internal::current_process_id()));
    std::error_code ec;fs::remove_all(root,ec);fs::create_directories(root/"src");
    std::ofstream(root/"src/main.cpp")<<"int main(){return 0;}\n";

    const auto value=phase4_assurance_test::contract("task-production-oracles");
    VerificationPlan plan;plan.metadata=value.metadata;plan.verification_plan_id="plan";
    OracleContext context{value.metadata,plan,value,nlohmann::json::object(),
                          "2026-08-12T00:00:00Z"};
    RepositoryEvidenceOracle repository(root,
        {{"system","artifact","src/main.cpp",true,true,std::nullopt}},"r1");
    assert(repository.production_ready()&&!repository.capability_manifest_digest().empty());
    auto repository_result=repository.collect(context);
    assert(repository_result.error.empty()&&repository_result.evidence.size()==1);
    assert(repository_result.evidence.front().outcome==FindingOutcome::Pass);
    const auto original_digest=repository_result.evidence.front().content_digest;
    std::ofstream(root/"src/main.cpp",std::ios::trunc)<<"int main(){return 1;}\n";
    repository_result=repository.collect(context);
    assert(repository_result.evidence.front().content_digest!=original_digest);
    fs::remove(root/"src/main.cpp",ec);
    repository_result=repository.collect(context);
    assert(repository_result.evidence.front().outcome==FindingOutcome::Fail);
    assert(!repository_result.findings.empty());

#if !defined(_WIN32)
    fs::create_directories(root/"src");std::ofstream(root/"src/main.cpp")<<"ok\n";
    sandbox::BubblewrapSandboxProvider provider({});
    std::string reason;
    if(provider.available(&reason)) {
        SandboxCommandOracle command(provider,root,
            {{"functional-test","functional","test",
              {"/bin/sh","-c","test -s /workspace/src/main.cpp"},5000,{0}}},
            "policy-r1","r1");
        assert(command.production_ready());
        const auto executed=command.collect(context);
        assert(executed.error.empty()&&executed.evidence.size()==1);
        assert(executed.evidence.front().outcome==FindingOutcome::Pass);
    }
#endif

    OracleRegistry registry;
    assert(registry.register_oracle(std::make_shared<RepositoryEvidenceOracle>(root,
        std::vector<RepositoryOracleRule>{{"system","artifact","src/main.cpp",true,true,std::nullopt}},"r1")));
    std::vector<std::string> issues;
    assert(!registry.production_ready(value,&issues));
    assert(!issues.empty()); // test/static_analysis/runtime/metric are fail-closed
    auto mismatched=phase4_assurance_test::contract("task-production-oracles");
    for(auto& criterion:mismatched.criteria)
        criterion.required_evidence={"artifact"};
    issues.clear();
    assert(!registry.production_ready(mismatched,&issues));
    assert(!issues.empty()); // artifact for system cannot cover another criterion
    assert(!registry.capability_manifest_digest().empty());
    fs::remove_all(root,ec);
}
