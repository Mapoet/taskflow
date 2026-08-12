#include "agent/harness/production_dependencies.hpp"

#include "agent/contracts/contract.hpp"

namespace agent_framework::harness {
ProductionDependencyReport validate_production_dependencies(
    const ProductionCompositionDependencies& d) {
    ProductionDependencyReport report;
    nlohmann::json manifest = nlohmann::json::object();
    auto require = [&](bool present, const char* name) {
        if(!present) report.issues.push_back({"production_dependency_missing", name,
            std::string("mandatory production dependency is missing: ") + name});
        manifest[name] = present;
    };
    require(d.harness_store, "harness_store"); require(d.run_store, "run_store");
    require(d.run_harness_saga, "run_harness_saga"); require(d.input_repository, "input_repository");
    require(d.cross_store_coordinator, "cross_store_coordinator");
    require(d.plan_store, "plan_store"); require(d.llm_store != nullptr, "llm_store");
    require(d.role_runtime != nullptr, "role_runtime"); require(d.telemetry != nullptr, "telemetry");
    require(d.approval_store, "approval_store"); require(d.sandbox_provider, "sandbox_provider");
    require(d.artifact_executor, "artifact_executor"); require(d.oracle_registry, "oracle_registry");
    require(d.assurance_store, "assurance_store"); require(d.memory_store, "memory_store");
    require(d.memory_workflow_store, "memory_workflow_store");
    require(d.remediation_store, "remediation_store"); require(d.judge_store, "judge_store");
    require(d.operations_assembler, "operations_assembler");
    require(d.input_assembler, "input_assembler");
    require(d.cognition_workflow, "cognition_workflow"); require(d.memory_workflow, "memory_workflow");
    require(d.assurance_workflow, "assurance_workflow");
    require(d.remediation_workflow, "remediation_workflow"); require(d.judge_workflow, "judge_workflow");
    require(!d.identity_verifier_manifest_digest.empty(), "identity_verifier_manifest");
    require(!d.policy_manifest_digest.empty(), "policy_manifest");
    require(!d.configuration_revision.empty(), "configuration_revision");
    if(d.sandbox_provider) {
        std::string reason;
        if(!d.sandbox_provider->available(&reason))
            report.issues.push_back({"sandbox_provider_unavailable", "sandbox_provider", reason});
        manifest["sandbox"]={{"id",d.sandbox_provider->id()},
                              {"version",d.sandbox_provider->version()}};
    }
    if(d.role_runtime && d.llm_store && d.role_runtime->store().get()!=d.llm_store.get())
        report.issues.push_back({"llm_store_identity_mismatch", "role_runtime",
            "RoleRuntime must use the declared durable LLM store"});
    if(d.cross_store_coordinator) {
        const auto digest=d.cross_store_coordinator->capability_manifest_digest();
        if(digest.empty()) report.issues.push_back({"coordination_manifest_invalid",
            "cross_store_coordinator", "coordination capability manifest is empty"});
        manifest["cross_store_coordination_manifest_digest"]=digest;
        std::vector<std::string> issues;
        if(!d.cross_store_coordinator->production_ready(&issues))
            for(const auto& issue:issues) report.issues.push_back({
                "coordination_participant_missing", "cross_store_coordinator", issue});
    }
    manifest["configuration_revision"]=d.configuration_revision;
    manifest["identity_verifier_manifest_digest"]=d.identity_verifier_manifest_digest;
    manifest["policy_manifest_digest"]=d.policy_manifest_digest;
    report.ready=report.issues.empty();
    if(report.ready) report.manifest_digest=contracts::canonical_digest(manifest).value_or("");
    return report;
}
}  // namespace agent_framework::harness
