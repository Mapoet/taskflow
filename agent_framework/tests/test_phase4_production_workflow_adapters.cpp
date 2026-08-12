#include <cassert>
#include <memory>

#include "agent/harness/production_workflow_adapters.hpp"
#include "phase4_harness_test_support.hpp"

int main() {
    using namespace agent_framework;
    using namespace agent_framework::harness;
    CallbackProductionWorkflowInputAssembler inputs;
    HarnessStageRequest request;
    request.checkpoint.metadata = phase4_harness_test::metadata();
    std::string error;
    assert(!inputs.cognition(request, &error) && !error.empty());
    error.clear(); assert(!inputs.memory(request, &error) && !error.empty());
    error.clear(); assert(!inputs.assurance(request, false, &error) && !error.empty());
    error.clear(); assert(!inputs.remediation(request, &error) && !error.empty());
    error.clear(); assert(!inputs.judge(request, &error) && !error.empty());

    auto store = std::make_shared<llm_runtime::InMemoryLLMRuntimeStore>();
    llm_runtime::LLMInvocationManifest manifest;
    manifest.metadata = phase4_harness_test::metadata();
    manifest.invocation_id = "invocation-a";
    manifest.state = llm_runtime::InvocationState::Pending;
    manifest.role = "planner";
    manifest.profile_id = "planner";
    manifest.profile_revision = "v1";
    manifest.prompt_id = "planner";
    manifest.prompt_revision = "v1";
    manifest.prompt_digest = "sha256:prompt";
    manifest.route_decision_digest = "sha256:route";
    manifest.candidate_id = "candidate";
    manifest.provider = "provider";
    manifest.model = "model";
    manifest.adapter_revision = "v1";
    manifest.reasoning_effort = "high";
    manifest.independence_group = "group-a";
    manifest.evidence_authority = "candidate";
    manifest.calibration_revision = "cal-v1";
    manifest.memory_snapshot_id = "snapshot";
    manifest.memory_view_profile = "planning";
    manifest.memory_view_digest = "sha256:view";
    manifest.capability_digest = "sha256:capability";
    manifest.input_digest = "sha256:input";
    manifest.output_digest = "sha256:output";
    manifest.started_at = "2026-08-12T00:00:00Z";
    manifest.finished_at = "2026-08-12T00:00:01Z";
    assert(store->create_invocation(manifest).ok());
    manifest.state = llm_runtime::InvocationState::Running;
    assert(store->update_invocation(manifest, 1).ok());
    manifest.state = llm_runtime::InvocationState::Succeeded;
    assert(store->update_invocation(manifest, 2).ok());
    InvocationManifestResolver resolver(store);
    error.clear();
    const auto found = resolver.resolve("tenant-a", {"invocation-a"}, &error);
    assert(error.empty() && found.size() == 1);
    error.clear();
    assert(resolver.resolve("tenant-a", {"missing"}, &error).empty() && !error.empty());
}
