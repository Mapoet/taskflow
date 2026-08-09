#include <cassert>
#include <string>
#include <vector>

#include "phase4_llm_runtime_test_support.hpp"

int main() {
    using namespace phase4_llm_test;

    const auto p = profile();
    const auto pr = prompt();
    const auto c = calibration();
    auto route = ModelRouteDecision{};
    route.metadata = metadata();
    route.route_id = "route-a";
    route.profile_id = p.profile_id;
    route.profile_revision = p.revision;
    route.selected = true;
    route.candidate_id = "primary";
    route.provider = "fake-primary";
    route.model = "model-a";
    route.adapter_revision = "adapter-r1";
    route.decision_code = "selected";

    LLMInvocationManifest manifest;
    manifest.metadata = metadata();
    manifest.invocation_id = "invocation-a";
    manifest.state = InvocationState::Succeeded;
    manifest.role = p.role;
    manifest.profile_id = p.profile_id;
    manifest.profile_revision = p.revision;
    manifest.prompt_id = pr.prompt_id;
    manifest.prompt_revision = pr.revision;
    manifest.prompt_digest = encode(pr).at("canonical_digest");
    manifest.output_digest = "sha256:accepted-output";

    ReasoningArtifact reasoning;
    reasoning.metadata = metadata();
    reasoning.artifact_id = "reasoning-a";
    reasoning.claims = {"claim-a"};
    reasoning.evidence_ids = {"evidence-a"};
    reasoning.confidence = 0.8;

    assert(decode_role_profile(encode(p)));
    assert(decode_prompt_revision(encode(pr)));
    assert(decode_route_decision(encode(route)));
    assert(decode_invocation_manifest(encode(manifest)));
    assert(decode_reasoning_artifact(encode(reasoning)));
    assert(decode_calibration_record(encode(c)));

    auto tampered = encode(p);
    tampered["payload"]["role"] = "tampered";
    std::vector<contracts::ContractIssue> issues;
    assert(!decode_role_profile(tampered, {}, &issues));
    assert(!issues.empty());

    auto unknown = encode(pr);
    unknown["payload"]["unknown_field"] = true;
    unknown["canonical_digest"] = *contracts::embedded_digest(unknown);
    assert(!decode_prompt_revision(unknown));

    auto invalid = p;
    invalid.provider_pool.clear();
    assert(!validate(invalid).empty());
    invalid = p;
    invalid.temperature = 3.0;
    assert(!validate(invalid).empty());
    invalid = p;
    invalid.provider_parameters["model"] = "audit-bypass";
    assert(!validate(invalid).empty());
    invalid = p;
    invalid.provider_parameters["api_key"] = "must-not-be-stored";
    assert(!validate(invalid).empty());

    const std::string durable = encode(manifest).dump();
    assert(durable.find("private-chain-of-thought") == std::string::npos);
    return 0;
}
